/*
 * ro_main.c
 *
 * Start-up, the icon bar icon, the Wimp poll loop and event dispatch, plus
 * the small helpers (error and confirmation boxes, placing a window at the
 * pointer) that the rest of the front end shares.
 *
 * The loop pumps the hotspot client on every pass: while a request is
 * running or the window is open it polls with a short idle time (the client
 * is a non-blocking state machine, so the desktop never waits on the
 * network); when there is nothing to do it blocks in Wimp_Poll with null
 * events masked out, using no CPU at all.
 */

#include "ro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "util.h"

app_state app;

#define ICON_SPRITE   "!hotspot"

/* Events we never want: pointer leaving/entering, caret changes and so on. */
#define MASK_UNWANTED \
    (wimp_MASK_LEAVING | wimp_MASK_ENTERING | wimp_MASK_LOSE | \
     wimp_MASK_GAIN | wimp_MASK_POLLWORD | wimp_MASK_ICON_LEAVING | \
     wimp_MASK_ICON_ENTERING)

/* ------------------------------------------------------------------ */
/* Message boxes                                                      */
/* ------------------------------------------------------------------ */

/* A plain error box uses the original form of Wimp_ReportError. One with an
 * icon category needs the "by category" form: it sets the registers the
 * category flag makes the Wimp read (sprite, area, buttons), which the plain
 * form leaves holding whatever they held. Return is OK and Escape is Cancel
 * (the highlight-Cancel flag would swap the two). */
static void report(wimp_error_box_flags flags, int by_category,
                   const char *fmt, va_list ap,
                   wimp_error_box_selection *click)
{
    os_error err;

    memset(&err, 0, sizeof err);
    err.errnum = 1;
    vsnprintf(err.errmess, sizeof err.errmess, fmt, ap);
    u_no_ctrl(err.errmess);     /* the Wimp ends the text at the first one */

    if (by_category)
        xwimp_report_error_by_category(&err, flags, APP_NAME, NULL, NULL, NULL,
                                       click);
    else
        xwimp_report_error(&err, flags, APP_NAME, click);
}

void ro_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(wimp_ERROR_BOX_OK_ICON, 0, fmt, ap, NULL);
    va_end(ap);
}

void ro_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    report(wimp_ERROR_BOX_OK_ICON | wimp_ERROR_BOX_NO_BEEP |
           (wimp_ERROR_BOX_CATEGORY_INFO << wimp_ERROR_BOX_CATEGORY_SHIFT),
           1, fmt, ap, NULL);
    va_end(ap);
}

int ro_confirm(const char *fmt, ...)
{
    va_list ap;
    wimp_error_box_selection click = wimp_ERROR_BOX_SELECTED_NOTHING;

    va_start(ap, fmt);
    report(wimp_ERROR_BOX_OK_ICON | wimp_ERROR_BOX_CANCEL_ICON |
           wimp_ERROR_BOX_NO_BEEP |
           (wimp_ERROR_BOX_CATEGORY_QUESTION << wimp_ERROR_BOX_CATEGORY_SHIFT),
           1, fmt, ap, &click);
    va_end(ap);

    return click == wimp_ERROR_BOX_SELECTED_OK;
}

/* ------------------------------------------------------------------ */
/* Windows and settings                                               */
/* ------------------------------------------------------------------ */

/* The size of the screen in OS units along one axis. */
static int mode_extent(os_mode_var limit, os_mode_var eig)
{
    int pixels = 0;
    int shift = 0;

    os_read_mode_variable(os_CURRENT_MODE, limit, &pixels);
    os_read_mode_variable(os_CURRENT_MODE, eig, &shift);

    return (pixels + 1) << shift;
}

/* Opens `w` (already created, `width` x `height` OS units) centred on the
 * pointer, kept on screen, in front of everything. */
void ro_open_at_pointer(wimp_w w, int width, int height)
{
    wimp_pointer p;
    wimp_window_state st;
    int screen_w;
    int screen_h;
    int x0;
    int y1;

    if (xwimp_get_pointer_info(&p) != NULL)
        return;

    screen_w = mode_extent(os_MODEVAR_XWIND_LIMIT, os_MODEVAR_XEIG_FACTOR);
    screen_h = mode_extent(os_MODEVAR_YWIND_LIMIT, os_MODEVAR_YEIG_FACTOR);

    x0 = p.pos.x - width / 2;
    y1 = p.pos.y + height / 2;

    if (x0 + width > screen_w - 20)
        x0 = screen_w - 20 - width;
    if (x0 < 20)
        x0 = 20;
    if (y1 > screen_h - 20)
        y1 = screen_h - 20;
    if (y1 - height < 140)
        y1 = height + 140;

    memset(&st, 0, sizeof st);
    st.w = w;
    if (xwimp_get_window_state(&st) != NULL)
        return;

    st.visible.x0 = x0;
    st.visible.y1 = y1;
    st.visible.x1 = x0 + width;
    st.visible.y0 = y1 - height;
    st.xscroll = 0;
    st.yscroll = 0;
    st.next = wimp_TOP;
    xwimp_open_window((wimp_open *)&st);
}

void ro_apply_config(const hs_config *cfg)
{
    const char *err;

    app.cfg = *cfg;
    hs_set_config(app.client, &app.cfg);
    app.cfg = *hs_get_config(app.client);       /* as sanitised */

    err = choices_save(&app.cfg);
    if (err != NULL)
        ro_error("%s", err);

    hs_refresh(app.client, win_visible_parts());
    win_model_changed(HS_R_ALL | HS_C_CONN);
}

/* ------------------------------------------------------------------ */
/* The icon bar icon                                                  */
/* ------------------------------------------------------------------ */

static char bar_text[16] = APP_NAME;
static char bar_valid[24] = "S" ICON_SPRITE;

static void create_bar_icon(void)
{
    wimp_icon_create ic;
    os_error *error;
    int w;

    memset(&ic, 0, sizeof ic);
    ic.w = wimp_ICON_BAR_RIGHT;

    /* The usual icon bar geometry (the RISC OS Style Guide's): the sprite
     * sits 20 units above the baseline, the text below it at -16. */
    w = (int)strlen(bar_text) * 16;
    if (w < 68)
        w = 68;

    ic.icon.extent.x0 = 0;
    ic.icon.extent.y0 = -16;
    ic.icon.extent.x1 = w;
    ic.icon.extent.y1 = 20 + 68;
    ic.icon.flags = wimp_ICON_TEXT | wimp_ICON_SPRITE | wimp_ICON_HCENTRED |
                    wimp_ICON_INDIRECTED |
                    (wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT) |
                    ((wimp_icon_flags)wimp_COLOUR_BLACK
                     << wimp_ICON_FG_COLOUR_SHIFT) |
                    ((wimp_icon_flags)wimp_COLOUR_VERY_LIGHT_GREY
                     << wimp_ICON_BG_COLOUR_SHIFT);
    ic.icon.data.indirected_text.text = bar_text;
    ic.icon.data.indirected_text.validation = bar_valid;
    ic.icon.data.indirected_text.size = (int)sizeof bar_text;

    error = xwimp_create_icon(&ic, &app.bar_icon);
    if (error != NULL)
        ro_error("Could not put the icon on the icon bar: %s", error->errmess);
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

static void handle_click(const wimp_pointer *p)
{
    if (p->w == wimp_ICON_BAR) {
        if (p->i != app.bar_icon)
            return;

        if (p->buttons & wimp_CLICK_MENU)
            menu_open_iconbar(p);
        else
            win_open();

        return;
    }

    if (dlg_owns(p->w))
        dlg_click(p);
    else if (p->w == win_handle())
        win_click(p);
}

static void handle_key(const wimp_key *k)
{
    if (dlg_owns(k->w))
        dlg_key(k);
    else if (k->w == win_handle())
        win_key(k);
    else
        xwimp_process_key(k->c);
}

static void handle_message(const wimp_message *msg)
{
    switch (msg->action) {
        case message_QUIT:
            app.quit = 1;
            break;

        case message_MENUS_DELETED:
            menu_deleted();
            break;

        default:
            break;
    }
}

static void dispatch(wimp_event_no event, wimp_block *block)
{
    switch (event) {
        case wimp_REDRAW_WINDOW_REQUEST:
            if (block->redraw.w == win_handle())
                win_redraw(&block->redraw);
            break;

        case wimp_OPEN_WINDOW_REQUEST:
            if (block->open.w == win_handle())
                win_open_request(&block->open);
            else
                xwimp_open_window(&block->open);
            break;

        case wimp_CLOSE_WINDOW_REQUEST:
            if (dlg_owns(block->close.w))
                dlg_close();
            else if (block->close.w == win_handle())
                win_close();
            break;

        case wimp_MOUSE_CLICK:
            handle_click(&block->pointer);
            break;

        case wimp_KEY_PRESSED:
            handle_key(&block->key);
            break;

        case wimp_MENU_SELECTION:
            menu_selection(&block->selection);
            break;

        case wimp_SCROLL_REQUEST:
            if (block->scroll.w == win_handle())
                win_scroll(&block->scroll);
            break;

        case wimp_USER_MESSAGE:
        case wimp_USER_MESSAGE_RECORDED:
            handle_message(&block->message);
            break;

        default:
            break;
    }
}

/* Reacts to what the client reports after a step. */
static void client_changed(unsigned changed)
{
    if (changed & (HS_R_ALL | HS_C_CONN | HS_C_ACTION | HS_C_SCAN))
        win_model_changed(changed);

    if (changed & HS_C_ACTION) {
        const hs_model *m = hs_get(app.client);

        /* A link that worked is remembered for next time. */
        act_action_finished(m->act_ok);

        /* Successes show in the window's "Last action" line; a failure
         * deserves a proper box. */
        if (!m->act_ok)
            ro_error("%s", m->act_msg);
    }

    /* The search for the hotspot ended: offer what it found. */
    if (changed & HS_C_SCAN)
        act_scan_results();
}

static void poll_loop(void)
{
    wimp_block block;
    wimp_event_no event;

    while (!app.quit) {
        os_t now = os_read_monotonic_time();
        int busy = hs_busy(app.client);
        int active = busy || win_is_open();
        os_error *error;

        if (active)
            error = xwimp_poll_idle(MASK_UNWANTED, &block,
                                    now + (busy ? 3 : 20), NULL, &event);
        else
            error = xwimp_poll(MASK_UNWANTED | wimp_MASK_NULL, &block, NULL,
                               &event);

        if (error != NULL) {
            ro_error("The Window Manager reported: %s", error->errmess);
            break;
        }

        dispatch(event, &block);

        client_changed(hs_step(app.client,
                               (unsigned long)os_read_monotonic_time()));
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    wimp_version_no version_out;
    os_error *error;
    int configured;

    http_init();

    error = xwimp_initialise(310, APP_NAME, NULL, &version_out, &app.task);
    if (error != NULL)
        return 1;

    configured = choices_load(&app.cfg);

    app.client = hs_new(&app.cfg);
    if (app.client == NULL) {
        ro_error("Out of memory.");
        xwimp_close_down(app.task);
        return 1;
    }

    app.cfg = *hs_get_config(app.client);

    win_init();
    info_init();
    create_bar_icon();

    /* First run: show the settings so the address (the shipped default is
     * only a starting point) can be checked and the password entered. */
    if (!configured || app.cfg.host[0] == '\0') {
        win_open();
        act_choices();
    }

    poll_loop();

    dlg_close();
    win_close();
    /* An open menu may be showing the Info window: close it before the window
     * is deleted, or the Wimp's menu stack would hold a dead handle. */
    xwimp_create_menu(wimp_NO_SUB_MENU, 0, 0);
    win_exit();
    menu_exit();
    info_exit();
    hs_free(app.client);
    xwimp_close_down(app.task);
    return 0;
}
