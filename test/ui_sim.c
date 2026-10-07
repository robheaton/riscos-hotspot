/*
 * ui_sim.c
 *
 * Plays a whole session against the real front end (ro_*.c, built
 * unchanged) on a fake desktop and a mock hotspot: first-run Choices, the
 * window filling in, Pause/Resume, the tabs, BrandMeister, links, system
 * actions, menus, scrolling, closing and reopening, quitting.
 *
 * The "user" finds things to click by looking at what the program actually
 * drew (the rectangles it handed to Wimp_PlotIcon), so a mismatch between
 * the drawing code and the hit-testing code shows up here as a click that
 * does nothing.
 *
 * usage: ui_sim PORT        (the mock hotspot must already be running)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fakewimp.h"
#include "http.h"
#include "ro.h"
#include "util.h"

int hotspot_main(void);

static int failures;
static int checks;
static int g_port;

#define LBL_ADDRESS "Hotspot address (an IP address such as 10.0.0.27)"

/* The width of the main window's rows (WORK_W in ro_win.c). */
#define WORK_WIDTH 1100

static void check_at(const char *file, int line, int ok, const char *expr)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(c) check_at(__FILE__, __LINE__, (c), #c)

/* ------------------------------------------------------------------ */
/* Talking to the mock hotspot                                        */
/* ------------------------------------------------------------------ */

static char *simple_get(const char *target)
{
    http_endpoint ep;
    http_req *r;
    char *out;
    size_t len;
    const char *body;
    unsigned long start = http_clock_cs();

    memset(&ep, 0, sizeof ep);
    u_copy(ep.host, sizeof ep.host, "127.0.0.1");
    ep.port = g_port;

    r = http_start(&ep, "GET", target, NULL, NULL, 0, 1u << 20, 500);
    while (http_step(r) != HTTP_DONE && http_step(r) != HTTP_FAILED) {
        if (http_clock_cs() - start > 500)
            break;
        usleep(1000);
    }

    body = http_body(r, &len);
    out = (char *)malloc(len + 1);
    memcpy(out, body, len);
    out[len] = '\0';
    http_free(r);
    return out;
}

static int mock_log_has(const char *needle)
{
    char *log = simple_get("/__mock/log");
    int found = strstr(log, needle) != NULL;

    free(log);
    return found;
}

static void mock_set(const char *setting)
{
    char target[200];

    snprintf(target, sizeof target, "/__mock/set?%s", setting);
    free(simple_get(target));
}

/* ------------------------------------------------------------------ */
/* The script                                                         */
/* ------------------------------------------------------------------ */

typedef int (*step_fn)(void);

static int step;
static unsigned long step_began;
static int sub;                 /* a step's own progress */

static unsigned long age(void)
{
    return http_clock_cs() - step_began;
}

static void next(void)
{
    step++;
    sub = 0;
    step_began = http_clock_cs();
}

/* Wait for `cond`; give up after `secs`. Returns 1 when the step is done. */
#define WAIT(cond, secs)                                                   \
    do {                                                                   \
        if (cond)                                                          \
            return 1;                                                      \
        if (age() > (unsigned long)((secs) * 100)) {                       \
            check_at(__FILE__, __LINE__, 0, "timed out waiting for " #cond);\
            return 1;                                                      \
        }                                                                  \
        return 0;                                                          \
    } while (0)

static wimp_w main_w(void)
{
    return fw_main_window()->w;
}

/* Set UI_SIM_DEBUG=1 to see everything the last redraw drew. */
static void dump_plots(const char *why)
{
    int i;

    if (getenv("UI_SIM_DEBUG") == NULL)
        return;

    printf("---- plots (%s): %d\n", why, fw_plot_count());
    for (i = 0; i < fw_plot_count(); i++) {
        const fw_plot *p = fw_plot_at(i);

        printf("  y=%5d..%5d x=%4d..%4d  \"%s\"\n", p->abs.y0, p->abs.y1,
               p->abs.x0, p->abs.x1, p->text);
    }
}

static int plot(const char *text)
{
    return fw_find_plot(text, 0);
}

static int row_button_nth(const char *row_text, int nth, const char *button)
{
    int anchor = fw_find_plot(row_text, nth);

    return (anchor < 0) ? -1 : fw_find_plot_on_row(button, anchor);
}

static int row_button(const char *row_text, const char *button)
{
    return row_button_nth(row_text, 0, button);
}

/* True if `text` is drawn on the same row as the first plot `row_text`. */
static int row_has(const char *row_text, const char *text)
{
    int anchor = fw_find_plot(row_text, 0);

    return anchor >= 0 && fw_find_plot_on_row(text, anchor) >= 0;
}

/* The first plot whose text starts with `prefix`; -1 if none. */
static int plot_prefix(const char *prefix)
{
    int i;

    for (i = 0; i < fw_plot_count(); i++) {
        if (strncmp(fw_plot_at(i)->text, prefix, strlen(prefix)) == 0)
            return i;
    }

    return -1;
}

/* Tabs are found on the row of the tab bar, so "DMR" cannot be mistaken
 * for a mode called DMR somewhere else on the page. */
static void click_tab(const char *name)
{
    int anchor = plot("Links");
    int p = (anchor < 0) ? -1 : fw_find_plot_on_row(name, anchor);

    CHECK(p >= 0);
    if (p >= 0)
        fw_click_plot(p, wimp_CLICK_SELECT);
}

/* Fixed-width icon text must fit its icon: a line wider than the dialogue
 * would be cut off on the real desktop (the system font is 16 OS units a
 * character). Writable fields scroll, so they are exempt; option buttons
 * give up some width to their tick. */
static void check_dialog_fits(const fw_window *dlg)
{
    int i;

    for (i = 0; i < dlg->nicons; i++) {
        const wimp_icon *ic = &dlg->icons[i];
        unsigned type = (ic->flags >> wimp_ICON_BUTTON_TYPE_SHIFT) & 0xF;
        int room;

        if (!(ic->flags & wimp_ICON_TEXT) || !(ic->flags & wimp_ICON_INDIRECTED))
            continue;
        if (type == wimp_BUTTON_WRITABLE)
            continue;

        room = ic->extent.x1 - ic->extent.x0;
        if (ic->flags & wimp_ICON_SPRITE)
            room -= 64;

        if ((int)strlen(ic->data.indirected_text.text) * 16 > room) {
            check_at(__FILE__, __LINE__, 0, "dialogue text fits its icon");
            printf("   too wide (%d > %d): \"%s\"\n",
                   (int)strlen(ic->data.indirected_text.text) * 16, room,
                   ic->data.indirected_text.text);
        }
    }
}

/* The newest dialogue; each one is checked for text that does not fit. */
static fw_window *dialog(void)
{
    static wimp_w seen;
    fw_window *dlg = fw_top_dialog();

    if (dlg != NULL && dlg->w != seen) {
        seen = dlg->w;
        check_dialog_fits(dlg);
    }

    return dlg;
}

static int file_has(const char *name, const char *needle)
{
    FILE *f = fopen(name, "r");
    char line[256];
    int found = 0;

    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, needle) != NULL)
            found = 1;
    }

    if (f != NULL)
        fclose(f);

    return found;
}

static void mock_reset(void)
{
    free(simple_get("/__mock/reset"));
}

/* Nothing the window draws may be cut off by its cell. A text wider than its
 * cell - at 16 OS units a character, the system font's width, which the
 * desktop font does not often exceed - and anything beyond the right-hand
 * edge of the rows is reported. */
static void check_not_clipped(const char *view)
{
    int i;

    for (i = 0; i < fw_plot_count(); i++) {
        const fw_plot *p = fw_plot_at(i);
        int need = (int)strlen(p->text) * 16;
        int room = p->rel.x1 - p->rel.x0;

        if (p->rel.x1 > WORK_WIDTH) {
            check_at(__FILE__, __LINE__, 0, "plot inside the rows' width");
            printf("   %s: \"%s\" ends at %d\n", view, p->text, p->rel.x1);
        }

        if (p->text[0] != '\0' && need > room) {
            check_at(__FILE__, __LINE__, 0, "text fits its cell");
            printf("   %s: \"%s\" needs %d, has %d\n", view, p->text, need,
                   room);
        }
    }
}

/* ---- step 0: first run -------------------------------------------- */

static int s_first_run(void)
{
    fw_window *dlg;
    char port[16];
    int i;

    /* As the program opens it: wide enough for the buttons against the
     * right-hand edge of the rows, and as tall as asked (the Wimp trims a
     * window to its extent, so the extent has to allow it). */
    {
        const wimp_window_state *st = &fw_main_window()->state;

        CHECK(st->visible.x1 - st->visible.x0 == WORK_WIDTH);
        CHECK(st->visible.y1 - st->visible.y0 == 900);
    }

    /* A tall window, so every row of the long Status list is drawn: the
     * user enlarging the window sends an open request the program has to
     * make room for. */
    {
        wimp_block b;

        memset(&b, 0, sizeof b);
        b.open.w = main_w();
        b.open.visible.x0 = 100;
        b.open.visible.y0 = 100;
        b.open.visible.x1 = 100 + WORK_WIDTH;
        b.open.visible.y1 = 3700;
        b.open.next = wimp_TOP;
        fw_queue(wimp_OPEN_WINDOW_REQUEST, &b);
    }

    CHECK(fw_bar_created);
    CHECK(strcmp(fw_bar_icon.data.indirected_text.text, "Hotspot") == 0);
    CHECK(strcmp(fw_bar_icon.data.indirected_text.validation, "S!hotspot") == 0);
    /* The same flags the real filers use for their icon bar icons. */
    CHECK(fw_bar_icon.flags == 0x1700310Bu);
    CHECK(fw_bar_icon.extent.y0 == -16 && fw_bar_icon.extent.y1 == 88);

    CHECK(fw_main_window() != NULL && fw_main_window()->open);

    dlg = dialog();
    CHECK(dlg != NULL);
    if (dlg == NULL)
        return 1;

    CHECK(dlg->open);
    CHECK(fw_caret_window == dlg->w);       /* typing goes into the dialogue */
    CHECK(fw_find_icon(dlg, LBL_ADDRESS) >= 0);

    /* The copy of Choices shipped with the program supplies the address,
     * but the first run still shows the settings, to check them and to
     * enter the password. */
    i = fw_find_icon(dlg, LBL_ADDRESS) + 1;
    CHECK(strcmp(fw_icon_text(dlg, i), "127.0.0.1") == 0);
    CHECK(fw_caret_icon >= 0);

    i = fw_find_icon(dlg, "Port") + 1;
    snprintf(port, sizeof port, "%d", g_port);
    CHECK(strcmp(fw_icon_text(dlg, i), port) == 0);

    i = fw_find_icon(dlg, "User name") + 1;
    CHECK(strcmp(fw_icon_text(dlg, i), "pi-star") == 0);   /* the default */

    i = fw_find_icon(dlg, "Password") + 1;
    CHECK(strstr(dlg->icons[i].data.indirected_text.validation, "D*") != NULL);
    fw_type(dlg, i, "raspberry");

    i = fw_find_icon(dlg, "Refresh every (seconds)") + 1;
    fw_type(dlg, i, "2");

    i = fw_find_icon(dlg, "Last heard rows to fetch") + 1;
    fw_type(dlg, i, "12");

    /* Switch the names option on, leave BrandMeister on. */
    i = fw_find_icon(dlg, "Show BrandMeister talkgroups");
    CHECK(i >= 0 && fw_icon_selected(dlg, i));
    i = fw_find_icon(dlg, "Look up operators' names (slower on the hotspot)");
    CHECK(i >= 0 && !fw_icon_selected(dlg, i));

    fw_click_icon(dlg, fw_find_icon(dlg, "Save"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_after_save(void)
{
    FILE *f;
    char line[200];
    int found_host = 0;
    int found_pass = 0;
    int found_refresh = 0;

    CHECK(dialog() == NULL);         /* closed again */
    /* ... and the main window has the keyboard back (F5 works at once). */
    CHECK(fw_caret_window == main_w());
    CHECK(file_has("<Hotspot$Dir>.Choices", "Configured=1"));
    CHECK(file_has("<Hotspot$Dir>.Choices", "RecentYSF=FCS00123,00002"));

    f = fopen("<Hotspot$Dir>.Choices", "r");
    CHECK(f != NULL);
    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "Host=127.0.0.1", 14) == 0)
            found_host = 1;
        if (strncmp(line, "Password=raspberry", 18) == 0)
            found_pass = 1;
        if (strncmp(line, "Refresh=2", 9) == 0)
            found_refresh = 1;
    }
    if (f != NULL)
        fclose(f);
    CHECK(found_host && found_pass && found_refresh);

    next();
    return 1;
}

/* ---- the window fills in ------------------------------------------ */

static int s_filled(void)
{
    /* The first poll fetches radio, last heard, status, then the system
     * cards: wait for the last of those to be on screen. */
    WAIT(plot("IDLE") >= 0 && plot("Connected.") >= 0 &&
         fw_find_plot("Pause", 2) >= 0 && plot("CPU Load") >= 0, 10);
}

static int s_check_status_view(void)
{
    char title[64];
    int p;
    const fw_plot *t;

    snprintf(title, sizeof title, "Hotspot  127.0.0.1:%d", g_port);
    CHECK(plot(title) >= 0);
    dump_plots("status view");
    check_not_clipped("Status");

    /* Values sit just right of the labels of their own section: close in
     * the Radio section, further over where a long label (the DMR master's
     * name) needs the room. */
    t = fw_plot_at(plot("438.800 MHz"));
    CHECK(t != NULL && t->rel.x0 <= 260);
    p = plot("BM 2341 United Kingdom");
    CHECK(p >= 0);
    if (p >= 0) {
        int v = fw_find_plot_on_row("active", p);

        CHECK(v >= 0 && fw_plot_at(v)->rel.x0 >= 380);
    }
    CHECK(plot("Modes Enabled") >= 0);
    CHECK(plot("Network Status") >= 0);
    CHECK(plot("DMR Master") >= 0);
    CHECK(plot("BM 2341 United Kingdom") >= 0);
    CHECK(plot("Radio") >= 0);
    CHECK(plot("438.800 MHz") >= 0);
    CHECK(plot("CPU Load") >= 0);

    /* Exactly three Pause buttons: D-Star, DMR, YSF. */
    CHECK(fw_find_plot("Pause", 2) >= 0);
    CHECK(fw_find_plot("Pause", 3) < 0);

    /* The tabs are there and "Status" is the selected one. */
    p = plot("Status");
    CHECK(p >= 0);
    t = fw_plot_at(p);
    CHECK(t != NULL && (t->flags & wimp_ICON_SELECTED));
    t = fw_plot_at(plot("Heard"));
    CHECK(t != NULL && !(t->flags & wimp_ICON_SELECTED));

    /* The row geometry is what the hit test assumes: buttons inside the
     * work area, right-aligned. */
    t = fw_plot_at(fw_find_plot("Pause", 0));
    CHECK(t != NULL && t->rel.x1 <= WORK_WIDTH && t->rel.x0 > 0);

    /* ... and within what the user can see, without scrolling sideways. */
    CHECK(t != NULL && t->abs.x1 <= fw_main_window()->state.visible.x1);
    CHECK(t != NULL && t->abs.x0 >= fw_main_window()->state.visible.x0);
    CHECK(fw_main_window()->state.visible.y1 -
          fw_main_window()->state.visible.y0 == 3600);

    next();
    return 1;
}

/* ---- Pause / Resume ------------------------------------------------ */

static int s_pause_dmr(void)
{
    int b;

    if (sub == 0) {
        b = row_button_nth("DMR", 1, "Pause");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("mode_action=Pause&mode_sel=DMR&func=mode_man") &&
         row_button_nth("DMR", 1, "Resume") >= 0, 10);
}

static int s_after_pause(void)
{
    CHECK(plot("Last action - Pause DMR: Paused: DMR Services are stopping...")
          >= 0);
    CHECK(fw_error_count == 0);
    next();
    return 1;
}

static int s_resume_dmr(void)
{
    int b;

    if (sub == 0) {
        b = row_button_nth("DMR", 1, "Resume");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("mode_action=Resume&mode_sel=DMR&func=mode_man") &&
         row_button_nth("DMR", 1, "Pause") >= 0, 10);
}

/* Adjust-click works as well as Select. */
static int s_pause_ysf_with_adjust(void)
{
    int b;

    if (sub == 0) {
        b = row_button("YSF", "Pause");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_ADJUST);
        sub = 1;
        return 0;
    }

    WAIT(row_button("YSF", "Resume") >= 0, 10);
}

static int s_resume_ysf(void)
{
    int b;

    if (sub == 0) {
        b = row_button("YSF", "Resume");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(row_button("YSF", "Pause") >= 0, 10);
}

/* ---- Heard --------------------------------------------------------- */

static int s_heard(void)
{
    if (sub == 0) {
        click_tab("Heard");
        sub = 1;
        return 0;
    }

    WAIT(plot("Last heard") >= 0 && plot("G4ABC") >= 0, 10);
}

static int s_check_heard(void)
{
    const fw_plot *t;
    int i;
    int n = 0;

    check_not_clipped("Heard");
    CHECK(plot("UTC") >= 0);
    CHECK(plot("07:12:11") >= 0);
    CHECK(plot("TG 91") >= 0);
    CHECK(plot("D-Star") >= 0);
    CHECK(plot("DMR S2") >= 0);                 /* "DMR Slot 2" shortened */
    CHECK(plot("4.2s 0% Net") >= 0);

    for (i = 0; i < fw_plot_count(); i++) {
        t = fw_plot_at(i);
        if (strcmp(t->text, "07:12:11") == 0 || strcmp(t->text, "07:11:40") == 0)
            n++;
    }
    CHECK(n >= 2);

    /* No Pause buttons on this tab. */
    CHECK(plot("Pause") < 0);

    next();
    return 1;
}

/* ---- Live transmission highlighting ------------------------------- */

static int s_rx(void)
{
    if (sub == 0) {
        mock_set("radio=RX%3A%20DMR");
        sub = 1;
        return 0;
    }

    /* The first row turns into a green "on air" row. */
    WAIT(plot("on air (Net)") >= 0, 10);
}

static int s_rx_done(void)
{
    const fw_plot *t = fw_plot_at(plot("on air (Net)"));

    CHECK(t != NULL && (t->flags & wimp_ICON_FILLED));
    mock_set("radio=IDLE");
    next();
    return 1;
}

/* ---- BrandMeister -------------------------------------------------- */

static int s_bm_tab(void)
{
    if (sub == 0) {
        click_tab("DMR");
        sub = 1;
        return 0;
    }

    WAIT(plot("TG 9990") >= 0, 10);
}

static int s_bm_link(void)
{
    int b;

    if (sub == 0) {
        CHECK(plot("TG 91") >= 0);
        CHECK(plot("Worldwide") >= 0);
        CHECK(plot("Add TG...") >= 0);
        CHECK(plot("Drop QSO") >= 0);
        CHECK(plot("Drop dynamic") >= 0);
        CHECK(plot("TG 2341") >= 0);
        CHECK(plot("UK Calling") >= 0);

        check_not_clipped("DMR");

        /* One DMR network, no TGIF: just BrandMeister's own list. */
        CHECK(plot("DMR networks") < 0);
        CHECK(plot_prefix("TGIF") < 0);

        b = row_button("TG 9990", "Link");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(row_button("TG 9990", "Drop") >= 0, 10);
}

static int s_bm_drop(void)
{
    int b;

    if (sub == 0) {
        b = row_button("TG 9990", "Drop");
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(row_button("TG 9990", "Link") >= 0, 10);
}

static int s_bm_add_open(void)
{
    fw_window *dlg;
    int i;

    if (sub == 0) {
        fw_click_plot(plot("Add TG..."), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    dlg = dialog();
    if (dlg == NULL)
        WAIT(0, 3);

    CHECK(fw_find_icon(dlg, "Talkgroup number(s)") >= 0);
    CHECK(fw_find_icon(dlg, "Timeslot (1 or 2)") < 0);     /* simplex */
    i = fw_find_icon(dlg, "Talkgroup number(s)") + 1;
    fw_type(dlg, i, "4321, 4322");
    fw_click_icon(dlg, fw_find_icon(dlg, "Add"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_bm_added(void)
{
    WAIT(plot("TG 4321") >= 0 && plot("TG 4322") >= 0 &&
         mock_log_has("TG=4321%2C+4322&TS=0&static-tg-add=Add+%26+Link"), 10);
}

static int s_bm_delete_cancel(void)
{
    int b;

    if (sub == 0) {
        b = row_button("TG 4321", "Delete");
        CHECK(b >= 0);
        fw_next_click = 2;                  /* Cancel in the confirmation */
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    /* Give it a moment: nothing must be sent. */
    if (age() < 150)
        return 0;

    CHECK(fw_error_count >= 1);
    CHECK((fw_errors[fw_error_count - 1].flags & wimp_ERROR_BOX_CANCEL_ICON) != 0);
    CHECK(strstr(fw_errors[fw_error_count - 1].msg, "Delete talkgroup 4321") != NULL);
    CHECK(!mock_log_has("droptg=4321"));
    CHECK(plot("TG 4321") >= 0);
    fw_next_click = 1;
    fw_error_count = 0;
    next();
    return 1;
}

static int s_bm_delete(void)
{
    int b;

    if (sub == 0) {
        b = row_button("TG 4321", "Delete");
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("droptg=4321&slot=0") && plot("TG 4321") < 0, 10);
}

static int s_bm_delete_other(void)
{
    int b;

    if (sub == 0) {
        b = row_button("TG 4322", "Delete");
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(plot("TG 4322") < 0, 10);
}

static int s_bm_drop_dynamic(void)
{
    if (sub == 0) {
        fw_click_plot(plot("Drop dynamic"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("cmd=drop_dynamic&slot=0") && plot("TG 2341") < 0, 10);
}

/* ---- DMRGateway: BrandMeister and TGIF side by side ---------------- */

static int s_gateway_on(void)
{
    if (sub == 0) {
        mock_set("gateway=1");
        mock_set("tgif=1");
        fw_click_plot(plot("Refresh"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(plot("DMR networks") >= 0 && plot("TGIF  2345678") >= 0 &&
         row_button("TGIF Network", "Disable") >= 0 &&
         row_button("BM 2341 United Kingdom", "Disable") >= 0 &&
         row_button("Timeslot 2", "Link...") >= 0, 10);
}

static int s_dmrnet_off(void)
{
    int b;

    if (sub == 0) {
        check_not_clipped("DMR (gateway)");
        CHECK(plot("Timeslot 1") < 0);          /* this one is simplex */
        CHECK(!row_has("Timeslot 2", "not linked"));
        CHECK(plot("TG 91  Worldwide") >= 0);
        mock_reset();
        b = row_button("TGIF Network", "Disable");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("dmrNet=net4&netState=disable") &&
         row_button("TGIF Network", "Enable") >= 0 &&
         plot_prefix("Last action - Disable TGIF Network") >= 0, 10);
}

static int s_dmrnet_on(void)
{
    int b;

    if (sub == 0) {
        mock_reset();
        b = row_button("TGIF Network", "Enable");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("dmrNet=net4&netState=enable") &&
         row_button("TGIF Network", "Disable") >= 0, 10);
}

static int s_tgif_link_open(void)
{
    fw_window *dlg;
    int i;

    if (sub == 0) {
        fw_click_plot(row_button("Timeslot 2", "Link..."), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    dlg = dialog();
    if (dlg == NULL)
        WAIT(0, 3);

    /* The timeslot is the row's: the dialogue only asks for the talkgroup. */
    CHECK(strstr(dlg->def.title_data.indirected_text.text,
                 "timeslot 2") != NULL);
    CHECK(fw_find_icon(dlg, "Timeslot (1 or 2)") < 0);
    i = fw_find_icon(dlg, "Talkgroup") + 1;
    CHECK(i > 0);
    mock_reset();
    fw_type(dlg, i, "2341");
    fw_click_icon(dlg, fw_find_icon(dlg, "Link"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_tgif_linked(void)
{
    WAIT(mock_log_has("tgifAction=LINK&tgifNumber=2341&tgifSlot=2") &&
         plot("TG 2341  UK Calling") >= 0 &&
         row_button("Recent talkgroups:", "TG 2341 TS2") >= 0, 10);
}

static int s_tgif_remembered(void)
{
    CHECK(file_has("<Hotspot$Dir>.Choices", "RecentTGIF=2341/2"));
    next();
    return 1;
}

static int s_tgif_unlink(void)
{
    int b;

    if (sub == 0) {
        mock_reset();
        b = row_button("Timeslot 2", "Unlink");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("tgifAction=UNLINK&tgifNumber=0&tgifSlot=2") &&
         row_has("Timeslot 2", "not linked") &&
         plot("TG 2341  UK Calling") < 0, 10);
}

/* One click puts back a talkgroup used before. */
static int s_tgif_relink_recent(void)
{
    int b;

    if (sub == 0) {
        mock_reset();
        b = row_button("Recent talkgroups:", "TG 2341 TS2");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("tgifAction=LINK&tgifNumber=2341&tgifSlot=2") &&
         plot("TG 2341  UK Calling") >= 0, 10);
}

/* The same switches are in the window's menu. */
static int s_dmr_menu(void)
{
    wimp_menu *root;
    wimp_menu *dmr_menu;
    int dmr;
    int items[3];

    if (sub == 0) {
        fw_click(main_w(), wimp_ICON_WINDOW, 900, 700, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        root = fw_menu;
        dmr = fw_menu_find(root, "DMR");
        CHECK(dmr == 2);
        CHECK(!(root->entries[dmr].icon_flags & wimp_ICON_SHADED));

        dmr_menu = root->entries[dmr].sub_menu;
        CHECK(dmr_menu != wimp_NO_SUB_MENU);
        CHECK(fw_menu_find(dmr_menu, "Disable BM 2341 United Kingdom") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Disable TGIF Network") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Add BM talkgroup...") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Drop BM QSO") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Link TGIF TS2...") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Unlink TGIF TS2") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Link TGIF TS1...") < 0);
        CHECK(fw_menu_find(dmr_menu, "Add BM talkgroup...") <
              fw_menu_find(dmr_menu, "Link TGIF TS2..."));

        mock_reset();
        items[0] = dmr;
        items[1] = fw_menu_find(dmr_menu, "Disable TGIF Network");
        items[2] = -1;
        fw_menu_select(items);
        sub = 2;
        return 0;
    }

    WAIT(mock_log_has("dmrNet=net4&netState=disable") &&
         row_button("TGIF Network", "Enable") >= 0, 10);
}

static int s_dmr_menu_on(void)
{
    wimp_menu *dmr_menu;
    int dmr;
    int items[3];

    if (sub == 0) {
        fw_click(main_w(), wimp_ICON_WINDOW, 900, 700, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        dmr = fw_menu_find(fw_menu, "DMR");
        dmr_menu = fw_menu->entries[dmr].sub_menu;
        CHECK(fw_menu_find(dmr_menu, "Enable TGIF Network") >= 0);
        CHECK(fw_menu_find(dmr_menu, "Disable TGIF Network") < 0);

        mock_reset();
        items[0] = dmr;
        items[1] = fw_menu_find(dmr_menu, "Enable TGIF Network");
        items[2] = -1;
        fw_menu_select(items);
        sub = 2;
        return 0;
    }

    WAIT(mock_log_has("dmrNet=net4&netState=enable") &&
         row_button("TGIF Network", "Disable") >= 0, 10);
}

/* Back to a hotspot with one network and no TGIF. */
static int s_gateway_off(void)
{
    if (sub == 0) {
        mock_set("gateway=0");
        mock_set("tgif=0");
        fw_click_plot(plot("Refresh"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(plot("DMR networks") < 0 && plot_prefix("TGIF") < 0 &&
         plot("TG 91") >= 0, 10);
}

/* ---- Links --------------------------------------------------------- */

static int s_links_tab(void)
{
    if (sub == 0) {
        click_tab("Links");
        sub = 1;
        return 0;
    }

    WAIT(plot("Reflectors and talkgroups") >= 0 && plot("YSF reflector") >= 0,
         10);
}

static int s_ysf_link(void)
{
    fw_window *dlg;
    int b;
    int i;

    if (sub == 0) {
        CHECK(plot("D-Star reflector") >= 0);
        CHECK(plot("P25 talkgroup") < 0);       /* not enabled on this hotspot */
        CHECK(plot("TGIF talkgroup") < 0);      /* TGIF is on the DMR tab */
        check_not_clipped("Links");

        /* What each mode is linked to now, from the hotspot's own page. */
        CHECK(plot("linked to UK-Calling") >= 0);
        CHECK(plot("linked to REF001 C") >= 0);
        /* The two remembered from the last session are there to click. */
        CHECK(row_button("Recent YSF reflectors:", "FCS00123") >= 0);
        CHECK(row_button("Recent YSF reflectors:", "00002") >= 0);
        CHECK(plot("Recent D-Star reflectors:") < 0);

        b = row_button("YSF reflector", "Link...");
        CHECK(b >= 0);
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    dlg = dialog();
    if (dlg == NULL)
        WAIT(0, 3);

    i = fw_find_icon(dlg, "Reflector") + 1;
    CHECK(i > 0);
    fw_type(dlg, i, "00001");
    fw_click_icon(dlg, fw_find_icon(dlg, "Link"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_ysf_linked(void)
{
    WAIT(mock_log_has("ysfLinkHost=YSF00001&Link=LINK&func=ysf_man") &&
         plot("linked to Test YSF00001") >= 0 &&
         row_button("Recent YSF reflectors:", "00001") >= 0, 10);
}

static int s_ysf_remembered(void)
{
    CHECK(file_has("<Hotspot$Dir>.Choices", "RecentYSF=00001,FCS00123,00002"));
    next();
    return 1;
}

static int s_dstar_link(void)
{
    fw_window *dlg;
    int b;
    int i;

    if (sub == 0) {
        b = row_button("D-Star reflector", "Link...");
        CHECK(b >= 0);
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    dlg = dialog();
    if (dlg == NULL)
        WAIT(0, 3);

    /* The radio module comes pre-filled from the status page's RPT1 pill. */
    i = fw_find_icon(dlg, "Your radio module (as the status page shows)") + 1;
    CHECK(i > 0);
    CHECK(strcmp(fw_icon_text(dlg, i), "M1ABC B") == 0);

    fw_type(dlg, fw_find_icon(dlg, "Reflector (such as REF001, XLX123, DCS001)") + 1,
            "xlx123");
    fw_type(dlg, fw_find_icon(dlg, "Module letter") + 1, "d");
    fw_click_icon(dlg, fw_find_icon(dlg, "Link"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_dstar_linked(void)
{
    WAIT((mock_log_has("Module=M1ABC+B&RefName=xlx123&Letter=d&Link=LINK&func=ds_man") ||
          mock_log_has("Module=M1ABC+B&RefName=XLX123&Letter=D&Link=LINK&func=ds_man")) &&
         row_button("Recent D-Star reflectors:", "XLX123 D") >= 0, 10);
}

static int s_ysf_unlink(void)
{
    int b;

    if (sub == 0) {
        b = row_button("YSF reflector", "Unlink");
        CHECK(b >= 0);
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("ysfLinkHost=none&Link=UNLINK&func=ysf_man"), 10);
}

static int s_dstar_unlink(void)
{
    int b;

    if (sub == 0) {
        b = row_button("D-Star reflector", "Unlink");
        CHECK(b >= 0);
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("Module=M1ABC+B&RefName=REF001&Letter=A&Link=UNLINK"), 10);
}

/* One click links a reflector used before. */
static int s_ysf_relink_recent(void)
{
    int b;

    if (sub == 0) {
        mock_reset();
        b = row_button("Recent YSF reflectors:", "00001");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("ysfLinkHost=YSF00001&Link=LINK&func=ysf_man") &&
         plot("linked to Test YSF00001") >= 0, 10);
}

static int s_dstar_relink_recent(void)
{
    int b;

    if (sub == 0) {
        mock_reset();
        b = row_button("Recent D-Star reflectors:", "XLX123 D");
        CHECK(b >= 0);
        if (b < 0)
            return 1;
        fw_click_plot(b, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    /* The radio module comes from the status page. */
    WAIT(mock_log_has("Module=M1ABC+B&RefName=XLX123&Letter=D&Link=LINK&func=ds_man"),
         10);
}

/* ---- System -------------------------------------------------------- */

static int s_system_tab(void)
{
    if (sub == 0) {
        click_tab("System");
        sub = 1;
        return 0;
    }

    WAIT(plot("Hotspot services") >= 0 && plot("Reboot") >= 0, 10);
}

static int s_reboot_cancel(void)
{
    if (sub == 0) {
        check_not_clipped("System");
        fw_error_count = 0;
        fw_next_click = 2;
        fw_click_plot(plot("Reboot"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    if (age() < 120)
        return 0;

    CHECK(fw_error_count == 1);
    CHECK(strstr(fw_errors[0].msg, "Reboot the hotspot") != NULL);
    /* OK and Cancel, and no highlight-Cancel: that would make Escape press
     * OK and Return Cancel. */
    CHECK((fw_errors[0].flags & wimp_ERROR_BOX_OK_ICON) != 0);
    CHECK((fw_errors[0].flags & wimp_ERROR_BOX_CANCEL_ICON) != 0);
    CHECK((fw_errors[0].flags & wimp_ERROR_BOX_HIGHLIGHT_CANCEL) == 0);
    CHECK(!mock_log_has("action=reboot"));
    fw_next_click = 1;
    fw_error_count = 0;
    next();
    return 1;
}

static int s_restart(void)
{
    if (sub == 0) {
        fw_click_plot(plot("Restart"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("action=restart_wpsd_services") &&
         plot("Last action - Restart services: Starting WPSD services...done") >= 0,
         10);
}

static int s_reboot_ok(void)
{
    if (sub == 0) {
        fw_click_plot(plot("Reboot"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(mock_log_has("action=reboot&format=json"), 10);
}

static int s_diagnostics(void)
{
    FILE *f;
    char *buf;
    long n;

    if (sub == 0) {
        fw_error_count = 0;
        fw_click_plot(plot("Save"), wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    CHECK(fw_error_count == 1);
    CHECK(strstr(fw_errors[0].msg, "Saved the diagnostics report") != NULL);

    f = fopen("<Hotspot$Dir>.Diagnostics", "r");
    CHECK(f != NULL);
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char *)malloc((size_t)n + 1);
        CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "request: GET /mmdvmhost/repeaterinfo.php") != NULL);
        CHECK(strstr(buf, "raspberry") == NULL);
        free(buf);
    }

    fw_error_count = 0;
    next();
    return 1;
}

/* ---- The Info window, the traditional way -------------------------- */

/* The icon bar menu's first entry is Info, with an arrow: the Program
 * information window is its submenu (the Wimp opens it when the pointer moves
 * onto the arrow and takes it away with the menu). The program never opens
 * it itself, and there is no longer an Info button in the System tab. */
static int s_info_window(void)
{
    wimp_menu *root;
    wimp_menu *submenu;
    fw_window *w;
    fw_window *shown;
    int items[2];
    int i;
    int day = 0;
    int year = 0;
    char month[8];
    char version[40];

    if (sub == 0) {
        CHECK(plot("Info") < 0);
        CHECK(plot("About Hotspot") < 0);
        fw_click(wimp_ICON_BAR, (wimp_i)7, 1200, 50, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        root = fw_menu;
        CHECK(root != NULL);
        if (root == NULL)
            return 1;

        CHECK(fw_menu_find(root, "Info") == 0);
        CHECK(!(root->entries[0].icon_flags & wimp_ICON_SHADED));

        /* The submenu is a window handle: not word aligned, which is how the
         * Wimp tells it from a menu block. */
        submenu = root->entries[0].sub_menu;
        CHECK(submenu != wimp_NO_SUB_MENU);
        CHECK(((intptr_t)submenu & 3) != 0);

        w = fw_window_by_handle((wimp_w)submenu);
        CHECK(w != NULL);
        if (w == NULL)
            return 1;

        CHECK(!w->open);                    /* the program does not open it */
        CHECK(dialog() == NULL);
        check_dialog_fits(w);

        /* A title bar and nothing else, moveable, redrawn by the Wimp: the
         * ROM program information windows' flags. */
        CHECK(strcmp(w->def.title_data.indirected_text.text,
                     "Program information") == 0);
        CHECK((w->def.flags & wimp_WINDOW_TITLE_ICON) != 0);
        CHECK((w->def.flags & wimp_WINDOW_MOVEABLE) != 0);
        CHECK((w->def.flags & wimp_WINDOW_AUTO_REDRAW) != 0);
        CHECK((w->def.flags & (wimp_WINDOW_BACK_ICON | wimp_WINDOW_CLOSE_ICON |
                               wimp_WINDOW_TOGGLE_ICON | wimp_WINDOW_VSCROLL |
                               wimp_WINDOW_HSCROLL | wimp_WINDOW_SIZE_ICON)) == 0);
        CHECK(w->extent.x1 - w->extent.x0 == 640);
        CHECK(w->extent.y1 - w->extent.y0 == 248);

        /* Name, Purpose, Author and Version, each a right-justified label
         * and an inset, centred field. */
        for (i = 0; i < 4; i++) {
            static const char *const labels[4] = { "Name", "Purpose", "Author",
                                                   "Version" };
            int l = fw_find_icon(w, labels[i]);

            CHECK(l >= 0 && l + 1 < w->nicons);
            if (l < 0)
                continue;

            CHECK((w->icons[l].flags & wimp_ICON_RJUSTIFIED) != 0);
            CHECK((w->icons[l + 1].flags & wimp_ICON_BORDER) != 0);
            CHECK((w->icons[l + 1].flags & wimp_ICON_HCENTRED) != 0);
            CHECK(strstr(w->icons[l + 1].data.indirected_text.validation,
                         "R2") != NULL);
            CHECK(((w->icons[l + 1].flags >> wimp_ICON_BUTTON_TYPE_SHIFT) & 0xF)
                  != wimp_BUTTON_WRITABLE);
        }

        CHECK(strcmp(fw_icon_text(w, fw_find_icon(w, "Name") + 1), "Hotspot") == 0);
        CHECK(strcmp(fw_icon_text(w, fw_find_icon(w, "Purpose") + 1),
                     APP_PURPOSE) == 0);
        /* "(c) Name, year", with the copyright sign as Latin-1 has it. */
        CHECK(strcmp(fw_icon_text(w, fw_find_icon(w, "Author") + 1),
                     APP_AUTHOR) == 0);
        CHECK((unsigned char)fw_icon_text(w, fw_find_icon(w, "Author") + 1)[0] ==
              0xA9);

        /* "0.03 (04 Oct 2026)": the version and the date it was built. */
        snprintf(version, sizeof version, "%s",
                 fw_icon_text(w, fw_find_icon(w, "Version") + 1));
        CHECK(strncmp(version, APP_VERSION " (", strlen(APP_VERSION) + 2) == 0);
        CHECK(sscanf(version + strlen(APP_VERSION) + 2, "%d %3s %d)", &day,
                     month, &year) == 3);
        CHECK(day >= 1 && day <= 31 && year >= 2026);
        CHECK(strstr("JanFebMarAprMayJunJulAugSepOctNovDec", month) != NULL);
        CHECK(version[strlen(version) - 1] == ')');

        /* The pointer moves onto the arrow: the Wimp opens the window. */
        shown = fw_menu_open_submenu_window(0);
        CHECK(shown == w && w->open);

        /* Choosing Info itself is not an action of ours. */
        items[0] = 0;
        items[1] = -1;
        fw_error_count = 0;
        fw_menu_select(items);
        sub = 2;
        return 0;
    }

    /* The menu is gone, and the window with it. */
    w = fw_window_by_handle((wimp_w)fw_menu->entries[0].sub_menu);
    CHECK(w != NULL && !w->open);
    CHECK(dialog() == NULL);
    CHECK(fw_error_count == 0);
    next();
    return 1;
}

/* ---- Text the desktop cannot show, and open requests --------------- */

/* Error box text with a new line or a colour code in it would be cut short by
 * the Wimp: it must arrive as plain text. */
static int s_error_text_plain(void)
{
    fw_error_count = 0;
    ro_error("first line\nsecond\tline\x1b!");
    CHECK(fw_error_count == 1);
    CHECK(strcmp(fw_errors[0].msg, "first line second line !") == 0);
    fw_error_count = 0;
    next();
    return 1;
}

static int extent_calls_before;

/* A drag of the scroll bar sends a stream of open requests for the same
 * window: none of them may redo the extent (which resets the scroll bars). */
static int s_open_requests_leave_extent(void)
{
    wimp_block b;
    fw_window *w = fw_main_window();
    int i;

    if (sub == 0) {
        extent_calls_before = fw_set_extent_count;

        for (i = 0; i < 30; i++) {
            memset(&b, 0, sizeof b);
            b.open.w = w->w;
            b.open.visible = w->state.visible;
            b.open.xscroll = w->state.xscroll;
            b.open.yscroll = w->state.yscroll;
            b.open.next = wimp_TOP;
            fw_queue(wimp_OPEN_WINDOW_REQUEST, &b);
        }

        sub = 1;
        return 0;
    }

    /* The script only runs when every request has been dealt with. */
    CHECK(fw_set_extent_count - extent_calls_before <= 1);
    next();
    return 1;
}

/* ---- Menus --------------------------------------------------------- */

static int s_window_menu(void)
{
    int items[4];
    int modes;
    int pause_ysf;
    wimp_menu *root;
    wimp_menu *sub_menu;

    if (sub == 0) {
        click_tab("Status");
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        if (plot("Modes Enabled") < 0)
            WAIT(0, 10);
        fw_click(main_w(), wimp_ICON_WINDOW, 900, 700, wimp_CLICK_MENU);
        sub = 2;
        return 0;
    }

    if (sub == 2) {
        root = fw_menu;
        CHECK(root != NULL);
        if (root == NULL)
            return 1;

        CHECK(strcmp(root->title_data.text, "Hotspot") == 0);
        CHECK(fw_menu_item_count(root) == 9);
        CHECK(fw_menu_find(root, "Refresh") == 0);
        CHECK(fw_menu_find(root, "Find hotspot...") == 6);
        CHECK(fw_menu_find(root, "Save diagnostics") == 7);
        CHECK(fw_menu_find(root, "Choices...") == 8);

        modes = fw_menu_find(root, "Modes");
        CHECK(modes == 1);
        CHECK(root->entries[modes].sub_menu != wimp_NO_SUB_MENU);
        CHECK(!(root->entries[modes].icon_flags & wimp_ICON_SHADED));

        sub_menu = root->entries[modes].sub_menu;
        pause_ysf = fw_menu_find(sub_menu, "Pause YSF");
        CHECK(pause_ysf >= 0);
        CHECK(fw_menu_find(sub_menu, "Pause P25") < 0);   /* not enabled */

        /* Last item flagged, width and height sane. */
        CHECK(root->entries[8].menu_flags & wimp_MENU_LAST);
        CHECK(root->width >= 16 * 7);
        CHECK(root->height == 44);

        items[0] = modes;
        items[1] = pause_ysf;
        items[2] = -1;
        fw_menu_select(items);
        sub = 3;
        return 0;
    }

    WAIT(row_button("YSF", "Resume") >= 0, 10);
}

static int s_window_menu_views(void)
{
    int items[4];
    wimp_menu *root;
    int views;

    if (sub == 0) {
        fw_click(main_w(), wimp_ICON_WINDOW, 900, 700, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    root = fw_menu;
    views = fw_menu_find(root, "View");
    CHECK(views == 5);
    CHECK(fw_menu_find(root->entries[views].sub_menu, "Status") == 0);
    CHECK(root->entries[views].sub_menu->entries[0].menu_flags & wimp_MENU_TICKED);

    /* View > Heard */
    items[0] = views;
    items[1] = 1;
    items[2] = -1;
    fw_menu_select(items);
    next();
    return 1;
}

static int s_menu_heard_shown(void)
{
    WAIT(plot("Last heard") >= 0 && plot("G4ABC") >= 0, 10);
}

static int open_choices_from_menu(void)
{
    int items[3];

    if (sub == 0) {
        fw_click(main_w(), wimp_ICON_WINDOW, 900, 700, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    items[0] = fw_menu_find(fw_menu, "Choices...");
    items[1] = -1;
    fw_menu_select(items);
    next();
    return 1;
}

static int s_menu_choices(void)
{
    return open_choices_from_menu();
}

static int s_choices_escape(void)
{
    fw_window *dlg = dialog();
    int i;

    if (dlg == NULL)
        WAIT(0, 3);

    /* The dialogue shows the saved values. */
    i = fw_find_icon(dlg, LBL_ADDRESS) + 1;
    CHECK(strcmp(fw_icon_text(dlg, i), "127.0.0.1") == 0);
    i = fw_find_icon(dlg, "Last heard rows to fetch") + 1;
    CHECK(strcmp(fw_icon_text(dlg, i), "12") == 0);

    /* Escape cancels: a changed field must not be applied. Tab has first
     * moved on to the next field, as the validation string says it may. */
    i = fw_find_icon(dlg, LBL_ADDRESS) + 1;
    fw_type(dlg, i, "10.1.2.3");
    fw_key(dlg->w, (wimp_i)i, wimp_KEY_TAB);
    CHECK(fw_caret_icon == fw_find_icon(dlg, "Port") + 1);
    fw_key(dlg->w, (wimp_i)fw_caret_icon, wimp_KEY_ESCAPE);
    next();
    return 1;
}

static int s_choices_cancelled(void)
{
    CHECK(dialog() == NULL);
    CHECK(strcmp(hs_get_config(app.client)->host, "127.0.0.1") == 0);
    next();
    return 1;
}

static int s_menu_choices_again(void)
{
    return open_choices_from_menu();
}

/* Slow the polling right down, so what follows can tell a click on Refresh
 * from a routine poll. Return moves from field to field, and in the last one
 * (not the Save button) saves. */
static int s_choices_save_slow(void)
{
    fw_window *dlg = dialog();
    int i;
    int last;

    if (dlg == NULL)
        WAIT(0, 3);

    if (sub == 0) {
        i = fw_find_icon(dlg, "Refresh every (seconds)") + 1;
        fw_type(dlg, i, "120");
        fw_key(dlg->w, (wimp_i)i, wimp_KEY_RETURN);
        sub = 1;
        return 0;
    }

    /* Return in the middle of the form went on to the next field, and did
     * not save yet. */
    last = fw_find_icon(dlg, "Last heard rows to fetch") + 1;
    CHECK(fw_caret_icon == last);
    fw_key(dlg->w, (wimp_i)last, wimp_KEY_RETURN);
    next();
    return 1;
}

static int s_settle(void)
{
    /* Let the poll that was already scheduled run, then reset the log. */
    if (sub == 0) {
        CHECK(dialog() == NULL);
        CHECK(hs_get_config(app.client)->refresh_s == 120);
        sub = 1;
        return 0;
    }

    if (age() < 400)
        return 0;

    free(simple_get("/__mock/reset"));
    next();
    return 1;
}

/* ---- the hotspot's address changes: Find hotspot ------------------- */

static int s_menu_choices_3(void)
{
    return open_choices_from_menu();
}

/* "The hotspot has moved": point the program at an address with nothing
 * listening. */
static int s_address_moved(void)
{
    fw_window *dlg = dialog();
    int i;

    if (dlg == NULL)
        WAIT(0, 3);

    i = fw_find_icon(dlg, LBL_ADDRESS) + 1;
    fw_type(dlg, i, "127.0.0.2");
    fw_click_icon(dlg, fw_find_icon(dlg, "Save"), wimp_CLICK_SELECT);
    next();
    return 1;
}

static int s_not_connected(void)
{
    /* The failure says which address it tried and offers the way out. */
    WAIT(plot_prefix("Not connected to 127.0.0.2: ") >= 0 &&
         fw_find_plot("Find hotspot...", 1) >= 0 &&
         plot("Connected.") < 0, 10);
}

static int s_find_bad_input(void)
{
    fw_window *dlg;
    int i;

    switch (sub) {
        case 0:
            fw_error_count = 0;
            fw_click_plot(fw_find_plot("Find hotspot...", 1),
                          wimp_CLICK_SELECT);
            sub = 1;
            return 0;

        case 1:
            dlg = dialog();
            if (dlg == NULL)
                WAIT(0, 3);

            /* Starts from the neighbourhood of the address in use. */
            i = fw_find_icon(dlg, "Search addresses starting with") + 1;
            CHECK(i > 0);
            CHECK(strcmp(fw_icon_text(dlg, i), "127.0.0.") == 0);
            CHECK(strstr(dlg->icons[i].data.indirected_text.validation,
                         "A0-9.") != NULL);

            fw_type(dlg, i, "12");          /* two numbers are not enough */

            /* A click on the main window does not take the keyboard away from
             * the dialogue the user is typing in. */
            fw_click(main_w(), wimp_ICON_WINDOW,
                     fw_main_window()->state.visible.x0 + 40,
                     fw_main_window()->state.visible.y1 - 12,
                     wimp_CLICK_SELECT);
            sub = 2;
            return 0;

        case 2:
            dlg = dialog();
            if (dlg == NULL)
                WAIT(0, 3);

            CHECK(fw_caret_window == dlg->w);
            fw_click_icon(dlg, fw_find_icon(dlg, "Search"), wimp_CLICK_SELECT);
            sub = 3;
            return 0;

        default:
            CHECK(fw_error_count == 1);
            CHECK(strstr(fw_errors[0].msg,
                         "not the start of an IP address") != NULL);
            CHECK(hs_get(app.client)->scan_state == 0);
            fw_error_count = 0;
            next();
            return 1;
    }
}

/* Found it, but the user says no: nothing changes. */
static int s_find_decline(void)
{
    fw_window *dlg;
    int i;

    switch (sub) {
        case 0:
            fw_click_plot(fw_find_plot("Find hotspot...", 0),
                          wimp_CLICK_SELECT);
            sub = 1;
            return 0;

        case 1:
            dlg = dialog();
            if (dlg == NULL)
                WAIT(0, 3);

            i = fw_find_icon(dlg, "Search addresses starting with") + 1;
            fw_type(dlg, i, "127.0.0.");
            fw_next_click = 2;              /* Cancel in the confirmation */
            fw_click_icon(dlg, fw_find_icon(dlg, "Search"), wimp_CLICK_SELECT);
            sub = 2;
            return 0;

        default:
            if (fw_error_count < 1)
                WAIT(0, 10);

            CHECK(fw_error_count == 1);
            CHECK(strstr(fw_errors[0].msg, "Found a hotspot at 127.0.0.1") != NULL);
            CHECK(strstr(fw_errors[0].msg, "MMDVM_HS_Hat") != NULL);
            CHECK((fw_errors[0].flags & wimp_ERROR_BOX_CANCEL_ICON) != 0);
            CHECK(strcmp(hs_get_config(app.client)->host, "127.0.0.2") == 0);
            CHECK(hs_get(app.client)->scan_state == 0);
            fw_next_click = 1;
            fw_error_count = 0;
            next();
            return 1;
    }
}

/* ... and accepted: the program moves to it and connects. */
static int s_find_accept(void)
{
    fw_window *dlg;
    int i;

    switch (sub) {
        case 0:
            fw_click_plot(fw_find_plot("Find hotspot...", 0),
                          wimp_CLICK_SELECT);
            sub = 1;
            return 0;

        case 1:
            dlg = dialog();
            if (dlg == NULL)
                WAIT(0, 3);

            i = fw_find_icon(dlg, "Search addresses starting with") + 1;
            fw_type(dlg, i, "127.0.0.");
            fw_click_icon(dlg, fw_find_icon(dlg, "Search"), wimp_CLICK_SELECT);
            sub = 2;
            return 0;

        default:
            WAIT(strcmp(hs_get_config(app.client)->host, "127.0.0.1") == 0 &&
                 plot("Connected.") >= 0 && plot_prefix("Not connected") < 0,
                 10);
    }
}

static int s_find_applied(void)
{
    CHECK(fw_error_count == 1);
    CHECK(strstr(fw_errors[0].msg, "Found a hotspot at 127.0.0.1") != NULL);
    CHECK((fw_errors[0].flags & wimp_ERROR_BOX_CANCEL_ICON) != 0);
    CHECK(file_has("<Hotspot$Dir>.Choices", "Host=127.0.0.1"));
    CHECK(hs_get(app.client)->scan_state == 0);
    fw_error_count = 0;
    next();
    return 1;
}

/* Nothing there: say so, and say what to check. */
static int s_find_none(void)
{
    fw_window *dlg;
    int i;

    switch (sub) {
        case 0:
            fw_click_plot(fw_find_plot("Find hotspot...", 0),
                          wimp_CLICK_SELECT);
            sub = 1;
            return 0;

        case 1:
            dlg = dialog();
            if (dlg == NULL)
                WAIT(0, 3);

            i = fw_find_icon(dlg, "Search addresses starting with") + 1;
            CHECK(strcmp(fw_icon_text(dlg, i), "127.0.0.") == 0);
            fw_type(dlg, i, "127.9.9");     /* no trailing dot needed */
            fw_click_icon(dlg, fw_find_icon(dlg, "Search"), wimp_CLICK_SELECT);
            sub = 2;
            return 0;

        default:
            if (fw_error_count < 1)
                WAIT(0, 10);

            CHECK(fw_error_count == 1);
            CHECK(strstr(fw_errors[0].msg,
                         "No hotspot was found at 127.9.9.1 - 127.9.9.254")
                  != NULL);
            CHECK(strcmp(hs_get_config(app.client)->host, "127.0.0.1") == 0);
            fw_error_count = 0;
            next();
            return 1;
    }
}

/* The search shows its progress and can be stopped. The hotspot is made
 * slow to answer, so that the one address that does answer keeps the search
 * going for a while after all the others have been refused: the progress
 * redraws stop with it, and the script (which only runs when the program has
 * nothing queued) gets its turn. It is started straight on the client; the
 * dialogue path has been exercised above. */
static int s_find_stop(void)
{
    int stop;

    switch (sub) {
        case 0:
            mock_set("delay_ms=2500");
            CHECK(hs_scan_start(app.client, "127.0.0.",
                                (unsigned long)http_clock_cs()) == 0);
            win_model_changed(HS_C_SCAN);
            sub = 1;
            step_began = http_clock_cs();
            return 0;

        case 1:
            /* All but the answering address have been tried. */
            if (hs_get(app.client)->scan_done < 240) {
                if (age() > 100)
                    CHECK(0 && "the search made no progress");
                return (age() > 100) ? 1 : 0;
            }

            CHECK(hs_get(app.client)->scan_state == 1);
            fw_redraw(main_w());
            sub = 2;
            return 0;

        case 2:
            stop = plot("Stop");
            CHECK(stop >= 0);
            CHECK(plot_prefix("Searching 127.0.0.1 - 254:") >= 0);
            if (stop >= 0)
                fw_click_plot(stop, wimp_CLICK_SELECT);
            sub = 3;
            return 0;

        default:
            CHECK(hs_get(app.client)->scan_state == 0);
            fw_redraw(main_w());
            if (sub == 3) {
                sub = 4;
                return 0;
            }
            CHECK(plot_prefix("Searching") < 0);
            CHECK(plot("Stop") < 0);
            CHECK(fw_error_count == 0);     /* stopped: nothing offered */
            mock_set("delay_ms=0");
            next();
            return 1;
    }
}

static int s_iconbar_menu(void)
{
    wimp_menu *root;

    if (sub == 0) {
        fw_pointer.pos.x = 1200;
        fw_pointer.pos.y = 50;
        fw_click(wimp_ICON_BAR, (wimp_i)7, 1200, 50, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    root = fw_menu;
    CHECK(root != NULL);
    CHECK(strcmp(root->title_data.text, "Hotspot") == 0);
    CHECK(fw_menu_item_count(root) == 4);
    CHECK(fw_menu_find(root, "Info") == 0);
    CHECK(fw_menu_find(root, "Show window") < 0);   /* clicking the icon does it */
    CHECK(fw_menu_find(root, "Find hotspot...") == 1);
    CHECK(fw_menu_find(root, "Choices...") == 2);
    CHECK(fw_menu_find(root, "Quit") == 3);
    /* The menu opens above the icon bar: its items end at the icon bar's
     * top edge, the title bar sitting above the position given. */
    CHECK(fw_menu_y == 96 + 4 * 44 + 24);
    next();
    return 1;
}

/* ---- Scrolling and window management ------------------------------ */

static int s_scroll(void)
{
    wimp_block b;
    fw_window *w = fw_main_window();
    int before;

    if (sub == 0) {
        /* Make the window short so there is something to scroll. */
        memset(&b, 0, sizeof b);
        b.open.w = w->w;
        b.open.visible.x0 = 100;
        b.open.visible.y0 = 400;
        b.open.visible.x1 = 100 + WORK_WIDTH;
        b.open.visible.y1 = 800;
        b.open.xscroll = 0;
        b.open.yscroll = 0;
        b.open.next = wimp_TOP;
        fw_queue(wimp_OPEN_WINDOW_REQUEST, &b);
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        CHECK(w->state.visible.y1 == 800);

        memset(&b, 0, sizeof b);
        b.scroll.w = w->w;
        b.scroll.visible = w->state.visible;
        b.scroll.xscroll = w->state.xscroll;
        b.scroll.yscroll = w->state.yscroll;
        b.scroll.next = wimp_TOP;
        b.scroll.ymin = wimp_SCROLL_LINE_DOWN;
        fw_queue(wimp_SCROLL_REQUEST, &b);
        sub = 2;
        return 0;
    }

    if (sub == 2) {
        before = w->state.yscroll;
        CHECK(before == -44);               /* one row down */

        memset(&b, 0, sizeof b);
        b.scroll.w = w->w;
        b.scroll.visible = w->state.visible;
        b.scroll.xscroll = w->state.xscroll;
        b.scroll.yscroll = w->state.yscroll;
        b.scroll.next = wimp_TOP;
        b.scroll.ymin = wimp_SCROLL_PAGE_DOWN;
        fw_queue(wimp_SCROLL_REQUEST, &b);
        sub = 3;
        return 0;
    }

    if (sub == 3) {
        CHECK(w->state.yscroll == -44 - (400 - 44));

        /* Scrolling far past the end is held at the end. */
        memset(&b, 0, sizeof b);
        b.scroll.w = w->w;
        b.scroll.visible = w->state.visible;
        b.scroll.xscroll = w->state.xscroll;
        b.scroll.yscroll = -100000;
        b.scroll.next = wimp_TOP;
        b.scroll.ymin = wimp_SCROLL_LINE_DOWN;
        fw_queue(wimp_SCROLL_REQUEST, &b);
        sub = 4;
        return 0;
    }

    CHECK(w->state.yscroll >= w->extent.y0 + 400);
    CHECK(w->state.yscroll <= 0);

    /* Back to the top, and the page up. */
    memset(&b, 0, sizeof b);
    b.scroll.w = w->w;
    b.scroll.visible = w->state.visible;
    b.scroll.xscroll = 0;
    b.scroll.yscroll = w->state.yscroll;
    b.scroll.next = wimp_TOP;
    b.scroll.ymin = wimp_SCROLL_PAGE_UP;
    fw_queue(wimp_SCROLL_REQUEST, &b);
    next();
    return 1;
}

static int s_status_again(void)
{
    if (sub == 0) {
        /* Back to the top first, then the Status tab. */
        wimp_block b;
        fw_window *w = fw_main_window();

        memset(&b, 0, sizeof b);
        b.open.w = w->w;
        b.open.visible.x0 = 100;
        b.open.visible.y0 = 100;
        b.open.visible.x1 = 100 + WORK_WIDTH;
        b.open.visible.y1 = 3700;
        b.open.next = wimp_TOP;
        fw_queue(wimp_OPEN_WINDOW_REQUEST, &b);
        fw_redraw(w->w);
        sub = 1;
        return 0;
    }

    if (sub == 1) {
        /* Enlarged again after being shrunk: the extent followed it. */
        CHECK(fw_main_window()->state.visible.y1 -
              fw_main_window()->state.visible.y0 == 3600);
        click_tab("Status");
        sub = 2;
        return 0;
    }

    WAIT(plot("Modes Enabled") >= 0, 10);
}

static int s_refresh_button(void)
{
    if (sub == 0) {
        int i = plot("Refresh");

        CHECK(i >= 0);
        if (i < 0)
            return 1;

        /* Polling is every two minutes now: only the click can bring this
         * change in within seconds. */
        mock_set("radio=TX%3A%20DMR");
        fw_click_plot(i, wimp_CLICK_SELECT);
        sub = 1;
        return 0;
    }

    WAIT(plot("TX: DMR") >= 0, 10);
}

static int s_f5(void)
{
    if (sub == 0) {
        /* Keys go to whoever owns the caret: the click on Refresh made the
         * window take it. */
        CHECK(fw_caret_window == main_w());
        mock_set("radio=OFFLINE");
        fw_key(main_w(), wimp_ICON_WINDOW, wimp_KEY_F5);
        sub = 1;
        return 0;
    }

    WAIT(plot("OFFLINE") >= 0, 10);
}

static int s_close_window(void)
{
    wimp_block b;
    char *log;

    switch (sub) {
        case 0:
            memset(&b, 0, sizeof b);
            b.close.w = main_w();
            fw_queue(wimp_CLOSE_WINDOW_REQUEST, &b);
            sub = 1;
            step_began = http_clock_cs();
            return 0;

        case 1:
            /* Let any request already in flight finish, then start with an
             * empty log. */
            if (age() < 250)
                return 0;
            CHECK(!fw_main_window()->open);
            free(simple_get("/__mock/reset"));
            sub = 2;
            step_began = http_clock_cs();
            return 0;

        case 2:
            /* Polling is every two seconds: several would have happened. */
            if (age() < 500)
                return 0;
            log = simple_get("/__mock/log");
            CHECK(strcmp(log, "[]") == 0);
            free(log);

            /* The icon bar icon opens the window again. */
            fw_click(wimp_ICON_BAR, (wimp_i)7, 1200, 50, wimp_CLICK_SELECT);
            sub = 3;
            step_began = http_clock_cs();
            return 0;

        default:
            log = simple_get("/__mock/log");
            if (fw_main_window()->open && strcmp(log, "[]") != 0) {
                free(log);
                return 1;
            }

            free(log);
            if (age() > 1000) {
                CHECK(0 && "polling did not resume after reopening");
                return 1;
            }

            return 0;
    }
}

static int s_quit_via_menu(void)
{
    int items[2];

    if (sub == 0) {
        fw_click(wimp_ICON_BAR, (wimp_i)7, 1200, 50, wimp_CLICK_MENU);
        sub = 1;
        return 0;
    }

    items[0] = fw_menu_find(fw_menu, "Quit");
    items[1] = -1;
    fw_menu_select(items);
    next();
    return 1;
}

static int s_wait_exit(void)
{
    /* The program should leave its poll loop by itself; if this step is ever
     * reached repeatedly it did not. */
    if (age() > 300) {
        CHECK(0 && "the program did not quit after Quit was chosen");
        return -1;
    }

    return 0;
}

static const step_fn steps[] = {
    s_first_run, s_after_save, s_filled, s_check_status_view,
    s_pause_dmr, s_after_pause, s_resume_dmr,
    s_pause_ysf_with_adjust, s_resume_ysf,
    s_heard, s_check_heard, s_rx, s_rx_done,
    s_bm_tab, s_bm_link, s_bm_drop, s_bm_add_open, s_bm_added,
    s_bm_delete_cancel, s_bm_delete, s_bm_delete_other, s_bm_drop_dynamic,
    s_gateway_on, s_dmrnet_off, s_dmrnet_on,
    s_tgif_link_open, s_tgif_linked, s_tgif_remembered, s_tgif_unlink,
    s_tgif_relink_recent, s_dmr_menu, s_dmr_menu_on, s_gateway_off,
    s_links_tab, s_ysf_link, s_ysf_linked, s_ysf_remembered,
    s_dstar_link, s_dstar_linked,
    s_ysf_unlink, s_dstar_unlink, s_ysf_relink_recent, s_dstar_relink_recent,
    s_system_tab, s_reboot_cancel, s_restart, s_reboot_ok, s_diagnostics,
    s_info_window, s_error_text_plain, s_open_requests_leave_extent,
    s_window_menu, s_window_menu_views, s_menu_heard_shown,
    s_close_window,
    s_menu_choices, s_choices_escape, s_choices_cancelled,
    s_menu_choices_again, s_choices_save_slow, s_settle,
    s_menu_choices_3, s_address_moved, s_not_connected, s_find_bad_input,
    s_find_decline, s_find_accept, s_find_applied, s_find_none, s_find_stop,
    s_iconbar_menu,
    s_scroll, s_status_again, s_refresh_button, s_f5,
    s_quit_via_menu, s_wait_exit,
    NULL
};

/* The step functions return 1 when finished; advance here so the WAIT
 * macros do not each have to. */
static int script_driver(void *ud)
{
    int before = step;
    int rc;

    (void)ud;

    if (steps[step] == NULL)
        return 0;

    rc = steps[step]();
    if (rc < 0)
        return 0;

    if (rc == 1 && step == before)
        next();

    return 1;
}

/* The copy of Choices that ships in the application directory: the usual
 * address of the hotspot (here, the mock) and the default login name. The
 * user has no choices of their own yet. */
static void write_shipped_defaults(void)
{
    FILE *f = fopen("<Hotspot$Dir>.Choices", "w");

    if (f == NULL)
        return;

    fprintf(f, "# defaults shipped with the application\n");
    fprintf(f, "Host=127.0.0.1\nPort=%d\nUser=pi-star\n", g_port);
    fprintf(f, "RecentYSF=FCS00123, 00002\n");     /* two it remembers */
    fclose(f);
}

int main(int argc, char **argv)
{
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s PORT\n", argv[0]);
        return 2;
    }

    g_port = atoi(argv[1]);

    /* Unbuffered, so what was reported before a crash is not lost. */
    setvbuf(stdout, NULL, _IONBF, 0);

    http_init();
    fw_reset();
    write_shipped_defaults();
    fw_set_script(script_driver, NULL);
    step_began = http_clock_cs();

    rc = hotspot_main();
    CHECK(rc == 0);
    CHECK(steps[step] != NULL);     /* ended by Quit, not by running out */

    /* Nothing the real Wimp would mishandle was asked of it, and no key was
     * sent where nobody was listening. */
    CHECK(fw_api_violations == 0);
    CHECK(fw_keys_lost == 0);
    CHECK(fw_menu == NULL);         /* closed before the Info window went */

    printf("%d checks, %d failures (finished at step %d of %d)\n", checks,
           failures, step, (int)(sizeof steps / sizeof steps[0]) - 1);
    return failures != 0;
}
