/*
 * fakewimp.c
 *
 * See fakewimp.h.
 */

#include "fakewimp.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "oslib/osfile.h"

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static fw_window windows[FW_MAX_WINDOWS];
static int       next_handle = 100;
static fw_window *menu_window;      /* a window the Wimp opened for a menu */

static fw_plot   plots[FW_MAX_PLOTS];
static int       nplots;
static fw_plot   frame[FW_MAX_PLOTS];      /* the last completed redraw */
static int       nframe;
static wimp_w    frame_window;

static int       redraw_active;
static wimp_w    redraw_window_handle;
static int       redraw_ox, redraw_oy;

#define QUEUE_MAX 64
static struct { wimp_event_no ev; wimp_block blk; } queue[QUEUE_MAX];
static int       qhead, qcount;

static fw_script_fn script;
static void        *script_ud;

int          fw_bar_created;
wimp_icon    fw_bar_icon;
wimp_menu   *fw_menu;
int          fw_menu_x, fw_menu_y;
int          fw_menu_count;
wimp_pointer fw_pointer;
int          fw_error_count;
fw_error     fw_errors[FW_MAX_ERRORS];
int          fw_next_click = 1;            /* OK */
wimp_w       fw_caret_window;
int          fw_keys_lost;
int          fw_api_violations;
int          fw_set_extent_count;
int          fw_caret_icon = -1;
int          fw_process_key_count;
int          fw_poll_count;

void fw_reset(void)
{
    int i;

    for (i = 0; i < FW_MAX_WINDOWS; i++)
        free(windows[i].icons);

    memset(windows, 0, sizeof windows);
    next_handle = 100;
    nplots = 0;
    nframe = 0;
    redraw_active = 0;
    qhead = 0;
    qcount = 0;
    script = NULL;
    fw_bar_created = 0;
    fw_menu = NULL;
    fw_menu_count = 0;
    fw_error_count = 0;
    fw_next_click = 1;
    fw_caret_icon = -1;
    fw_caret_window = 0;
    fw_keys_lost = 0;
    fw_api_violations = 0;
    fw_set_extent_count = 0;
    fw_process_key_count = 0;
    fw_poll_count = 0;
    memset(&fw_pointer, 0, sizeof fw_pointer);
}

/* ------------------------------------------------------------------ */
/* Event queue and polling                                            */
/* ------------------------------------------------------------------ */

void fw_set_script(fw_script_fn fn, void *ud)
{
    script = fn;
    script_ud = ud;
}

void fw_queue(wimp_event_no event, const wimp_block *block)
{
    int slot;

    if (qcount >= QUEUE_MAX) {
        fprintf(stderr, "fakewimp: event queue overflow\n");
        abort();
    }

    slot = (qhead + qcount) % QUEUE_MAX;
    queue[slot].ev = event;
    if (block != NULL)
        queue[slot].blk = *block;
    else
        memset(&queue[slot].blk, 0, sizeof queue[slot].blk);
    qcount++;
}

void fw_message_quit(void)
{
    wimp_block b;

    memset(&b, 0, sizeof b);
    b.message.size = 20;
    b.message.action = message_QUIT;
    fw_queue(wimp_USER_MESSAGE, &b);
}

static os_error *poll_common(wimp_block *block, wimp_event_no *event)
{
    fw_poll_count++;

    if (qcount == 0 && script != NULL) {
        if (!script(script_ud)) {
            script = NULL;
            fw_message_quit();
        }
    }

    if (qcount > 0) {
        *event = queue[qhead].ev;
        *block = queue[qhead].blk;
        qhead = (qhead + 1) % QUEUE_MAX;
        qcount--;
        return NULL;
    }

    usleep(1000);
    *event = wimp_NULL_REASON_CODE;
    memset(block, 0, sizeof *block);
    return NULL;
}

os_error *xwimp_poll_idle(wimp_poll_flags mask, wimp_block *block, os_t t,
                          int *pollword, wimp_event_no *event)
{
    (void)mask;
    (void)t;
    (void)pollword;
    return poll_common(block, event);
}

os_error *xwimp_poll(wimp_poll_flags mask, wimp_block *block, int *pollword,
                     wimp_event_no *event)
{
    (void)mask;
    (void)pollword;
    return poll_common(block, event);
}

os_error *xwimp_initialise(wimp_version_no version, char const *name,
                           wimp_message_list const *messages,
                           wimp_version_no *version_out, wimp_t *task_out)
{
    (void)version;
    (void)name;
    (void)messages;

    if (version_out != NULL)
        *version_out = 380;
    *task_out = (wimp_t)(intptr_t)4242;
    return NULL;
}

os_error *xwimp_close_down(wimp_t t)
{
    (void)t;
    return NULL;
}

os_error *xwimp_process_key(wimp_key_no c)
{
    (void)c;
    fw_process_key_count++;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Windows                                                            */
/* ------------------------------------------------------------------ */

fw_window *fw_window_by_handle(wimp_w w)
{
    int i;

    for (i = 0; i < FW_MAX_WINDOWS; i++) {
        if (windows[i].used && !windows[i].deleted && windows[i].w == w)
            return &windows[i];
    }

    return NULL;
}

fw_window *fw_main_window(void)
{
    int i;

    for (i = 0; i < FW_MAX_WINDOWS; i++) {
        if (windows[i].used && !windows[i].deleted)
            return &windows[i];
    }

    return NULL;
}

fw_window *fw_top_dialog(void)
{
    int i;
    fw_window *best = NULL;
    fw_window *main_win = fw_main_window();

    /* A window that exists but has not been opened (the program information
     * window, waiting to be somebody's submenu) is not on the screen. */
    for (i = 0; i < FW_MAX_WINDOWS; i++) {
        if (windows[i].used && !windows[i].deleted && windows[i].open &&
            &windows[i] != main_win)
            best = &windows[i];
    }

    return best;
}

int fw_icon_count(const fw_window *win)
{
    return win->nicons;
}

os_error *xwimp_create_window(wimp_window const *window, wimp_w *w)
{
    int i;
    fw_window *fw = NULL;
    const wimp_window_base *def = (const wimp_window_base *)window;
    const wimp_icon *icons;

    for (i = 0; i < FW_MAX_WINDOWS; i++) {
        if (!windows[i].used) {
            fw = &windows[i];
            break;
        }
    }

    if (fw == NULL) {
        static os_error err = { 1, "fakewimp: too many windows" };

        return &err;
    }

    memset(fw, 0, sizeof *fw);
    fw->used = 1;
    /* The real Wimp's handles are an address plus one: never word aligned,
     * which is how Wimp_CreateMenu tells a window (a dialogue box) from a
     * menu block. */
    fw->w = (wimp_w)(intptr_t)(next_handle++ * 8 + 1);
    fw->def = *def;
    fw->extent = def->extent;
    fw->state.w = fw->w;
    fw->state.visible = def->visible;
    fw->state.xscroll = def->xscroll;
    fw->state.yscroll = def->yscroll;
    fw->state.next = def->next;
    fw->state.flags = def->flags;
    fw->nicons = def->icon_count;

    if (fw->nicons > 0) {
        icons = (const wimp_icon *)((const char *)window +
                                    sizeof(wimp_window_base));
        fw->icons = (wimp_icon *)malloc((size_t)fw->nicons * sizeof(wimp_icon));
        memcpy(fw->icons, icons, (size_t)fw->nicons * sizeof(wimp_icon));
    }

    *w = fw->w;
    return NULL;
}

os_error *xwimp_delete_window(wimp_w w)
{
    fw_window *fw = fw_window_by_handle(w);

    if (fw != NULL) {
        fw->deleted = 1;
        fw->open = 0;
        if (fw_caret_window == w) {
            fw_caret_window = 0;
            fw_caret_icon = -1;
        }
    }

    return NULL;
}

/* Wimp_OpenWindow keeps the visible area inside the extent by adjusting the
 * scroll offsets. */
static void clamp_scroll(fw_window *fw)
{
    int vis_h = fw->state.visible.y1 - fw->state.visible.y0;
    int vis_w = fw->state.visible.x1 - fw->state.visible.x0;
    int ymax = fw->extent.y1;
    int ymin = fw->extent.y0 + vis_h;
    int xmin = fw->extent.x0;
    int xmax = fw->extent.x1 - vis_w;

    if (ymin > ymax)
        ymin = ymax;
    if (xmax < xmin)
        xmax = xmin;

    if (fw->state.yscroll > ymax)
        fw->state.yscroll = ymax;
    if (fw->state.yscroll < ymin)
        fw->state.yscroll = ymin;
    if (fw->state.xscroll > xmax)
        fw->state.xscroll = xmax;
    if (fw->state.xscroll < xmin)
        fw->state.xscroll = xmin;
}

os_error *xwimp_open_window(wimp_open *open)
{
    fw_window *fw = fw_window_by_handle(open->w);

    if (fw == NULL) {
        static os_error err = { 2, "fakewimp: bad window handle" };

        return &err;
    }

    fw->state.visible = open->visible;
    fw->state.xscroll = open->xscroll;
    fw->state.yscroll = open->yscroll;
    fw->state.next = open->next;
    fw->state.flags |= wimp_WINDOW_OPEN;
    fw->open = 1;

    /* The real Wimp never opens a window larger than its extent (and never
     * smaller than its minimum size): it trims the right and bottom edges. */
    {
        int vis_w = fw->state.visible.x1 - fw->state.visible.x0;
        int vis_h = fw->state.visible.y1 - fw->state.visible.y0;
        int ext_w = fw->extent.x1 - fw->extent.x0;
        int ext_h = fw->extent.y1 - fw->extent.y0;

        if (vis_w < fw->def.xmin)
            vis_w = fw->def.xmin;
        if (vis_h < fw->def.ymin)
            vis_h = fw->def.ymin;
        if (vis_w > ext_w)
            vis_w = ext_w;
        if (vis_h > ext_h)
            vis_h = ext_h;

        fw->state.visible.x1 = fw->state.visible.x0 + vis_w;
        fw->state.visible.y0 = fw->state.visible.y1 - vis_h;
    }

    clamp_scroll(fw);

    /* The Wimp redraws newly exposed parts; just ask for a full redraw. */
    if (!fw->redraw_queued && (fw->def.flags & wimp_WINDOW_AUTO_REDRAW) == 0) {
        wimp_block b;

        memset(&b, 0, sizeof b);
        b.redraw.w = fw->w;
        fw_queue(wimp_REDRAW_WINDOW_REQUEST, &b);
        fw->redraw_queued = 1;
    }

    return NULL;
}

os_error *xwimp_close_window(wimp_w w)
{
    fw_window *fw = fw_window_by_handle(w);

    if (fw != NULL) {
        fw->open = 0;
        fw->state.flags &= ~wimp_WINDOW_OPEN;
    }

    return NULL;
}

os_error *xwimp_get_window_state(wimp_window_state *state)
{
    fw_window *fw = fw_window_by_handle(state->w);

    if (fw == NULL) {
        static os_error err = { 3, "fakewimp: bad window handle" };

        return &err;
    }

    *state = fw->state;
    return NULL;
}

os_error *xwimp_set_extent(wimp_w w, os_box const *box)
{
    fw_window *fw = fw_window_by_handle(w);

    fw_set_extent_count++;

    if (fw != NULL)
        fw->extent = *box;

    return NULL;
}

os_error *xwimp_force_redraw(wimp_w w, int x0, int y0, int x1, int y1)
{
    fw_window *fw = fw_window_by_handle(w);

    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;

    if (fw != NULL && fw->open && !fw->redraw_queued &&
        (fw->def.flags & wimp_WINDOW_AUTO_REDRAW) == 0) {
        wimp_block b;

        memset(&b, 0, sizeof b);
        b.redraw.w = w;
        fw_queue(wimp_REDRAW_WINDOW_REQUEST, &b);
        fw->redraw_queued = 1;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* The redraw loop                                                    */
/* ------------------------------------------------------------------ */

os_error *xwimp_redraw_window(wimp_draw *redraw, osbool *more)
{
    fw_window *fw = fw_window_by_handle(redraw->w);

    if (fw == NULL) {
        static os_error err = { 4, "fakewimp: bad window handle (redraw)" };

        *more = 0;
        return &err;
    }

    fw->redraw_queued = 0;

    redraw->box = fw->state.visible;
    redraw->xscroll = fw->state.xscroll;
    redraw->yscroll = fw->state.yscroll;
    redraw->clip = fw->state.visible;

    redraw_active = 1;
    redraw_window_handle = fw->w;
    redraw_ox = redraw->box.x0 - redraw->xscroll;
    redraw_oy = redraw->box.y1 - redraw->yscroll;
    nplots = 0;

    *more = 1;
    return NULL;
}

os_error *xwimp_get_rectangle(wimp_draw *draw, osbool *more)
{
    (void)draw;

    /* One rectangle only: finish the frame. */
    memcpy(frame, plots, (size_t)nplots * sizeof plots[0]);
    nframe = nplots;
    frame_window = redraw_window_handle;
    redraw_active = 0;

    *more = 0;
    return NULL;
}

os_error *xwimp_plot_icon(wimp_icon const *icon)
{
    fw_plot *p;
    const char *text;
    size_t n;

    if (!redraw_active)
        return NULL;

    if (nplots >= FW_MAX_PLOTS) {
        fprintf(stderr, "fakewimp: too many plots in one redraw\n");
        abort();
    }

    p = &plots[nplots++];
    memset(p, 0, sizeof *p);
    p->rel = icon->extent;
    p->abs.x0 = icon->extent.x0 + redraw_ox;
    p->abs.x1 = icon->extent.x1 + redraw_ox;
    p->abs.y0 = icon->extent.y0 + redraw_oy;
    p->abs.y1 = icon->extent.y1 + redraw_oy;
    p->flags = icon->flags;

    if (icon->flags & wimp_ICON_INDIRECTED)
        text = icon->data.indirected_text.text;
    else
        text = icon->data.text;

    if (text != NULL) {
        for (n = 0; n + 1 < sizeof p->text && (unsigned char)text[n] >= 32;
             n++)
            p->text[n] = text[n];
        p->text[n] = '\0';
    }

    return NULL;
}

int fw_plot_count(void)
{
    return nframe;
}

const fw_plot *fw_plot_at(int i)
{
    return (i >= 0 && i < nframe) ? &frame[i] : NULL;
}

int fw_find_plot(const char *text, int nth)
{
    int i;

    for (i = 0; i < nframe; i++) {
        if (strcmp(frame[i].text, text) == 0 && nth-- == 0)
            return i;
    }

    return -1;
}

int fw_find_plot_on_row(const char *text, int anchor)
{
    int i;

    if (anchor < 0 || anchor >= nframe)
        return -1;

    for (i = 0; i < nframe; i++) {
        if (strcmp(frame[i].text, text) == 0 &&
            frame[i].abs.y0 < frame[anchor].abs.y1 &&
            frame[i].abs.y1 > frame[anchor].abs.y0)
            return i;
    }

    return -1;
}

void fw_redraw(wimp_w w)
{
    wimp_block b;
    fw_window *fw = fw_window_by_handle(w);

    /* Deliver the redraw request now, by running a poll that returns it. */
    (void)fw;
    memset(&b, 0, sizeof b);
    b.redraw.w = w;
    fw_queue(wimp_REDRAW_WINDOW_REQUEST, &b);
}

/* ------------------------------------------------------------------ */
/* Icons                                                              */
/* ------------------------------------------------------------------ */

os_error *xwimp_create_icon(wimp_icon_create const *icon, wimp_i *i)
{
    fw_bar_created = 1;
    fw_bar_icon = icon->icon;
    *i = 7;
    return NULL;
}

os_error *xwimp_get_icon_state(wimp_icon_state *st)
{
    fw_window *fw = fw_window_by_handle(st->w);

    if (fw == NULL || st->i < 0 || st->i >= fw->nicons) {
        static os_error err = { 5, "fakewimp: bad icon" };

        return &err;
    }

    st->icon = fw->icons[st->i];
    return NULL;
}

os_error *xwimp_set_caret_position(wimp_w w, wimp_i i, int x, int y,
                                   int height, int index)
{
    fw_window *fw = fw_window_by_handle(w);

    (void)x;
    (void)y;
    (void)height;
    (void)index;

    /* The real Wimp refuses a caret in a window that is not open. */
    if (fw == NULL || !fw->open) {
        static os_error err = { 6, "fakewimp: caret in a window that is not open" };

        return &err;
    }

    fw_caret_window = w;
    fw_caret_icon = (int)i;
    return NULL;
}

static const char *icon_text_of(const wimp_icon *ic)
{
    if (ic->flags & wimp_ICON_INDIRECTED)
        return ic->data.indirected_text.text;

    return ic->data.text;
}

int fw_find_icon(const fw_window *win, const char *text)
{
    int i;

    for (i = 0; i < win->nicons; i++) {
        const char *t = icon_text_of(&win->icons[i]);
        size_t n = 0;

        if (t == NULL)
            continue;

        while ((unsigned char)t[n] >= 32)
            n++;

        if (strlen(text) == n && strncmp(t, text, n) == 0)
            return i;
    }

    return -1;
}

const char *fw_icon_text(const fw_window *win, int i)
{
    static char buf[200];
    const char *t = icon_text_of(&win->icons[i]);
    size_t n = 0;

    if (t == NULL)
        return "";

    while (n + 1 < sizeof buf && (unsigned char)t[n] >= 32) {
        buf[n] = t[n];
        n++;
    }

    buf[n] = '\0';
    return buf;
}

/* Like the Wimp: edit the buffer in place and end the text with a CR. */
void fw_type(fw_window *win, int i, const char *text)
{
    wimp_icon *ic = &win->icons[i];
    char *buf = ic->data.indirected_text.text;
    int size = ic->data.indirected_text.size;
    size_t n = strlen(text);

    /* To type into a field the user has put the caret in it. */
    fw_caret_window = win->w;
    fw_caret_icon = i;

    if (n > (size_t)size - 2)
        n = (size_t)size - 2;

    memcpy(buf, text, n);
    buf[n] = '\r';
    buf[n + 1] = '\0';
}

int fw_icon_selected(const fw_window *win, int i)
{
    return (win->icons[i].flags & wimp_ICON_SELECTED) != 0;
}

/* ------------------------------------------------------------------ */
/* Menus and messages                                                 */
/* ------------------------------------------------------------------ */

/* An entry's submenu is a menu block (word aligned), nothing (-1) or the
 * handle of a window the Wimp will open for it; Wimp_CreateMenu itself can
 * likewise be given a window. A handle that is not a live window would make
 * the real Wimp report "bad window handle" (or worse). */
/* The Wimp takes a window it opened for a menu away again when the menu goes. */
static void close_menu_window(void)
{
    if (menu_window != NULL) {
        menu_window->open = 0;
        menu_window->state.flags &= ~wimp_WINDOW_OPEN;
        menu_window = NULL;
    }
}

static void check_submenu(const wimp_menu *sub, int depth)
{
    intptr_t v = (intptr_t)sub;

    if (sub == wimp_NO_SUB_MENU)
        return;

    if ((v & 3) != 0) {
        fw_window *w = fw_window_by_handle((wimp_w)v);

        if (w == NULL) {
            printf("fakewimp: a menu refers to window handle %lx, which is not "
                   "a live window\n", (unsigned long)v);
            fw_api_violations++;
        } else if (w->open) {
            printf("fakewimp: a menu refers to a window that is already open\n");
            fw_api_violations++;
        }

        return;
    }

    if (depth < 6) {
        int i;
        int n = fw_menu_item_count(sub);

        for (i = 0; i < n; i++)
            check_submenu(sub->entries[i].sub_menu, depth + 1);
    }
}

os_error *xwimp_create_menu(wimp_menu *menu, int x, int y)
{
    /* -1 just closes whatever menus are open. */
    if (menu == wimp_NO_SUB_MENU) {
        close_menu_window();
        fw_menu = NULL;
        return NULL;
    }

    if (menu != NULL)
        check_submenu(menu, 0);

    close_menu_window();

    fw_menu = menu;
    fw_menu_x = x;
    fw_menu_y = y;
    fw_menu_count++;
    return NULL;
}

/* The pointer has moved onto the arrow of item `item` of the open menu: for
 * an entry whose submenu is a window the Wimp opens that window itself (at
 * the right of the entry). Returns the window, or NULL if the entry has no
 * window submenu. */
fw_window *fw_menu_open_submenu_window(int item)
{
    intptr_t v;
    fw_window *w;

    if (fw_menu == NULL || item < 0 || item >= fw_menu_item_count(fw_menu))
        return NULL;

    v = (intptr_t)fw_menu->entries[item].sub_menu;
    if (fw_menu->entries[item].sub_menu == wimp_NO_SUB_MENU || (v & 3) == 0)
        return NULL;

    w = fw_window_by_handle((wimp_w)v);
    if (w != NULL) {
        close_menu_window();
        w->open = 1;
        w->state.flags |= wimp_WINDOW_OPEN;
        menu_window = w;
    }

    return w;
}

int fw_menu_item_count(const wimp_menu *m)
{
    int n = 0;

    while (!(m->entries[n].menu_flags & wimp_MENU_LAST))
        n++;

    return n + 1;
}

const char *fw_menu_item_text(const wimp_menu *m, int i)
{
    return m->entries[i].data.indirected_text.text;
}

int fw_menu_find(const wimp_menu *m, const char *text)
{
    int i;
    int n = fw_menu_item_count(m);

    for (i = 0; i < n; i++) {
        if (strcmp(fw_menu_item_text(m, i), text) == 0)
            return i;
    }

    return -1;
}

void fw_menu_select(const int *items)
{
    wimp_block b;
    int i;

    close_menu_window();        /* a selection closes the menus */

    memset(&b, 0, sizeof b);
    for (i = 0; i < 9; i++) {
        b.selection.items[i] = items[i];
        if (items[i] < 0)
            break;
    }

    fw_queue(wimp_MENU_SELECTION, &b);
}

static os_error *record_error(os_error const *error, wimp_error_box_flags flags,
                              wimp_error_box_selection *click)
{
    /* With both OK and Cancel and "highlight Cancel" set, the real Wimp
     * swaps what Return and Escape do, so Escape would press OK. */
    if ((flags & wimp_ERROR_BOX_OK_ICON) && (flags & wimp_ERROR_BOX_CANCEL_ICON) &&
        (flags & wimp_ERROR_BOX_HIGHLIGHT_CANCEL)) {
        printf("fakewimp: Wimp_ReportError with OK, Cancel and highlight-Cancel: "
               "Escape would confirm\n");
        fw_api_violations++;
    }

    /* The Wimp lays an error box's text out only up to the first byte below
     * 32: anything after a new line would simply not be shown. */
    {
        const unsigned char *p;

        for (p = (const unsigned char *)error->errmess; *p != '\0'; p++) {
            if (*p < 0x20) {
                printf("fakewimp: error box text has a control character (%d): "
                       "the Wimp would cut it short\n", *p);
                fw_api_violations++;
                break;
            }
        }
    }

    if (fw_error_count < FW_MAX_ERRORS) {
        snprintf(fw_errors[fw_error_count].msg,
                 sizeof fw_errors[fw_error_count].msg, "%s", error->errmess);
        fw_errors[fw_error_count].flags = flags;
    }

    fw_error_count++;

    if (click != NULL)
        *click = (wimp_error_box_selection)fw_next_click;

    return NULL;
}

os_error *xwimp_report_error(os_error const *error, wimp_error_box_flags flags,
                             char const *name, wimp_error_box_selection *click)
{
    (void)name;

    /* The plain veneer sets only R0-R2. Bit 8 ("new style") makes the Wimp
     * read R3-R5 - sprite, area, button list - which are then garbage. */
    if (flags & wimp_ERROR_BOX_GIVEN_CATEGORY) {
        printf("fakewimp: new-style flags passed to the plain Wimp_ReportError "
               "(R3-R5 would be garbage)\n");
        fw_api_violations++;
    }

    return record_error(error, flags, click);
}

os_error *xwimp_report_error_by_category(os_error const *error,
                                         wimp_error_box_flags flags,
                                         char const *name,
                                         char const *sprite_name,
                                         osspriteop_area const *area,
                                         char const *buttons,
                                         wimp_error_box_selection *click)
{
    (void)name;
    (void)sprite_name;
    (void)area;
    (void)buttons;

    /* The SWI is called with bit 8 set. */
    return record_error(error, flags | wimp_ERROR_BOX_GIVEN_CATEGORY, click);
}

os_error *xos_gs_trans(char const *s, char *buffer, int size, int *used,
                       bits *psr)
{
    size_t n = strlen(s);

    if (n > (size_t)size)
        n = (size_t)size;

    memcpy(buffer, s, n);
    if (used != NULL)
        *used = (int)n;
    if (psr != NULL)
        *psr = 0;

    return NULL;
}

os_error *xwimp_get_pointer_info(wimp_pointer *pointer)
{
    *pointer = fw_pointer;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Playing the user                                                   */
/* ------------------------------------------------------------------ */

void fw_click(wimp_w w, wimp_i i, int x, int y, int buttons)
{
    wimp_block b;

    memset(&b, 0, sizeof b);
    b.pointer.pos.x = x;
    b.pointer.pos.y = y;
    b.pointer.buttons = (wimp_mouse_state)buttons;
    b.pointer.w = w;
    b.pointer.i = i;

    fw_pointer = b.pointer;
    fw_queue(wimp_MOUSE_CLICK, &b);
}

void fw_click_plot(int plot_index, int buttons)
{
    const fw_plot *p = fw_plot_at(plot_index);

    if (p == NULL) {
        fprintf(stderr, "fakewimp: no such plot %d\n", plot_index);
        abort();
    }

    fw_click(frame_window, wimp_ICON_WINDOW, (p->abs.x0 + p->abs.x1) / 2,
             (p->abs.y0 + p->abs.y1) / 2, buttons);
}

void fw_click_icon(fw_window *win, int i, int buttons)
{
    wimp_icon *ic = &win->icons[i];
    unsigned type = (ic->flags >> wimp_ICON_BUTTON_TYPE_SHIFT) & 0xF;

    /* A radio/option icon on its own toggles when clicked. */
    if (type == wimp_BUTTON_RADIO)
        ic->flags ^= wimp_ICON_SELECTED;

    fw_click(win->w, (wimp_i)i,
             (win->state.visible.x0 + ic->extent.x0 + ic->extent.x1) / 2,
             win->state.visible.y1 +
                 (ic->extent.y0 + ic->extent.y1) / 2 - win->state.yscroll,
             buttons);
}

/* The next writable icon after `from` in the window, or -1. */
static int next_writable(const fw_window *fw, int from)
{
    int k;

    for (k = from + 1; k < fw->nicons; k++) {
        unsigned type = (fw->icons[k].flags >> wimp_ICON_BUTTON_TYPE_SHIFT) & 0xF;

        if (type == wimp_BUTTON_WRITABLE)
            return k;
    }

    return -1;
}

/* Does the icon's validation string have a K command containing `letter`
 * ("Ktar": t = Tab, a = arrow keys, r = Return move between fields)? */
static int has_key_command(const wimp_icon *ic, char letter)
{
    const char *v;

    if (!(ic->flags & wimp_ICON_INDIRECTED) ||
        ic->data.indirected_text.validation == NULL)
        return 0;

    for (v = ic->data.indirected_text.validation; *v != '\0';) {
        if ((v[0] == 'K' || v[0] == 'k') && (v == ic->data.indirected_text.validation ||
                                              v[-1] == ';')) {
            const char *p;

            for (p = v + 1; *p != '\0' && *p != ';'; p++) {
                if (*p == letter)
                    return 1;
            }
        }

        while (*v != '\0' && *v != ';')
            v++;
        if (*v == ';')
            v++;
    }

    return 0;
}

/* Keys go to the window that owns the caret, whatever the script says: the
 * real Wimp has no other idea of where the keyboard is. Inside a window,
 * Return and Tab in a field whose validation says so move the caret to the
 * next writable icon instead of being delivered. */
void fw_key(wimp_w w, wimp_i i, int key)
{
    wimp_block b;
    fw_window *fw = fw_window_by_handle(w);

    (void)i;

    if (w != fw_caret_window || fw == NULL) {
        fw_keys_lost++;
        return;
    }

    if (fw_caret_icon >= 0 && fw_caret_icon < fw->nicons) {
        const wimp_icon *ic = &fw->icons[fw_caret_icon];
        int next = next_writable(fw, fw_caret_icon);

        if (next >= 0 &&
            ((key == wimp_KEY_RETURN && has_key_command(ic, 'r')) ||
             (key == wimp_KEY_TAB && has_key_command(ic, 't')))) {
            fw_caret_icon = next;
            return;
        }
    }

    memset(&b, 0, sizeof b);
    b.key.w = w;
    b.key.i = (wimp_i)fw_caret_icon;
    b.key.c = (wimp_key_no)key;
    fw_queue(wimp_KEY_PRESSED, &b);
}

/* ------------------------------------------------------------------ */
/* Operating system                                                   */
/* ------------------------------------------------------------------ */

os_t os_read_monotonic_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (os_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000L);
}

bits os_read_mode_variable(os_mode mode, os_mode_var var, int *var_val)
{
    (void)mode;

    switch (var) {
        case os_MODEVAR_XWIND_LIMIT: *var_val = 1919; break;
        case os_MODEVAR_YWIND_LIMIT: *var_val = 1079; break;
        case os_MODEVAR_XEIG_FACTOR:
        case os_MODEVAR_YEIG_FACTOR: *var_val = 1; break;
        default: *var_val = 0; break;
    }

    return 0;
}

int os_read_var_val_size(char const *var, int context, os_var_type var_type,
                         int *used, os_var_type *var_type_out)
{
    (void)var;
    (void)context;
    (void)var_type;

    *used = 0;                  /* "Choices$Write" is not set */
    if (var_type_out != NULL)
        *var_type_out = 0;
    return 0;
}

os_error *xosfile_read_no_path(char const *file_name,
                               fileswitch_object_type *obj_type,
                               bits *load_addr, bits *exec_addr, int *size,
                               fileswitch_attr *attr)
{
    struct stat st;

    (void)load_addr;
    (void)exec_addr;
    (void)size;
    (void)attr;

    if (stat(file_name, &st) != 0)
        *obj_type = fileswitch_NOT_FOUND;
    else if (S_ISDIR(st.st_mode))
        *obj_type = fileswitch_IS_DIR;
    else
        *obj_type = fileswitch_IS_FILE;

    return NULL;
}

os_error *xosfile_set_type(char const *file_name, bits file_type)
{
    (void)file_name;
    (void)file_type;
    return NULL;
}

os_error *xosfile_create_dir(char const *dir_name, int entry_count)
{
    (void)entry_count;
    mkdir(dir_name, 0777);
    return NULL;
}
