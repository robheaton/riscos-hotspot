/*
 * ro_win.c
 *
 * The main window. Its work area is a plain scrolling list of rows (built
 * by rows.c) that this file draws itself in the redraw loop, one
 * Wimp_PlotIcon per text cell or button, and hit-tests by hand on clicks.
 * That keeps the number of rows free to change on every refresh, which a
 * window made of fixed icons could not do.
 *
 * Inside a redraw loop Wimp_PlotIcon takes icon rectangles in work area
 * coordinates (it adds the window origin itself), so the same numbers are
 * used for drawing and for converting a click back into a row and column.
 */

#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define ROW_H       44
#define WORK_W      1100        /* the width of the rows, and of the window */
#define PAD         16
#define BTN_MIN_W   110
#define BTN_GAP     10
#define CELL_INSET  4

/* The window opens as wide as the rows are laid out: the buttons sit against
 * the right-hand edge of the work area, so a narrower window would hide them
 * until it was scrolled or enlarged. */
#define WIN_X0      100
#define WIN_Y0      100
#define WIN_W       WORK_W
#define WIN_H       900

#define CHAR_W      16          /* system font character width, OS units */

/* Label/value rows: the values start just right of the longest label in the
 * run of such rows, between these limits. */
#define LABEL_GAP   32
#define LABEL_MIN_X 224
#define LABEL_MAX_X 460

static wimp_w    win;
static int       opened;
static ui_view   view = VIEW_STATUS;
static ui_rows   rows;
static ui_rows   prev;
static char      title_text[32] = "Hotspot";
static int       extent_h = WIN_H;      /* the extent's height, as last set */

/* ------------------------------------------------------------------ */
/* Creation, opening, closing                                         */
/* ------------------------------------------------------------------ */

wimp_w win_handle(void)
{
    return win;
}

int win_is_open(void)
{
    return opened;
}

ui_view win_view(void)
{
    return view;
}

/* Keys go to whichever window owns the caret, and this one has no writable
 * icon to put it in, so it takes an invisible one: otherwise F5 would never
 * reach it. (Bit 25 of the height word makes the caret invisible.) */
static void claim_caret(void)
{
    xwimp_set_caret_position(win, wimp_ICON_WINDOW, 0, 0, 0x02000000, 0);
}

void win_claim_caret(void)
{
    if (opened)
        claim_caret();
}

unsigned win_visible_parts(void)
{
    return HS_R_RADIO | HS_R_STATUS | ui_view_focus(view);
}

void win_init(void)
{
    wimp_window_base def;
    os_error *error;

    ui_rows_init(&rows);
    ui_rows_init(&prev);

    memset(&def, 0, sizeof def);

    def.visible.x0 = WIN_X0;
    def.visible.y0 = WIN_Y0;
    def.visible.x1 = WIN_X0 + WIN_W;
    def.visible.y1 = WIN_Y0 + WIN_H;
    def.xscroll = 0;
    def.yscroll = 0;
    def.next = wimp_TOP;

    def.flags =
        wimp_WINDOW_NEW_FORMAT |
        wimp_WINDOW_MOVEABLE |
        wimp_WINDOW_BACK_ICON |
        wimp_WINDOW_CLOSE_ICON |
        wimp_WINDOW_TITLE_ICON |
        wimp_WINDOW_TOGGLE_ICON |
        wimp_WINDOW_VSCROLL |
        wimp_WINDOW_HSCROLL |
        wimp_WINDOW_SIZE_ICON |
        wimp_WINDOW_SCROLL_REPEAT;      /* we scroll it a row at a time */

    def.title_fg = wimp_COLOUR_BLACK;
    def.title_bg = wimp_COLOUR_LIGHT_GREY;
    def.work_fg = wimp_COLOUR_BLACK;
    def.work_bg = wimp_COLOUR_VERY_LIGHT_GREY;
    def.scroll_outer = wimp_COLOUR_MID_LIGHT_GREY;
    def.scroll_inner = wimp_COLOUR_VERY_LIGHT_GREY;
    def.highlight_bg = wimp_COLOUR_CREAM;
    def.extra_flags = 0;

    /* The Wimp never opens a window larger than its extent, so the extent
     * starts out as big as the window. */
    def.extent.x0 = 0;
    def.extent.y0 = -WIN_H;
    def.extent.x1 = WORK_W;
    def.extent.y1 = 0;

    def.title_flags =
        wimp_ICON_TEXT |
        wimp_ICON_INDIRECTED |
        wimp_ICON_BORDER |
        wimp_ICON_HCENTRED |
        wimp_ICON_VCENTRED |
        wimp_ICON_FILLED |
        ((wimp_icon_flags)wimp_COLOUR_BLACK << wimp_ICON_FG_COLOUR_SHIFT) |
        ((wimp_icon_flags)wimp_COLOUR_LIGHT_GREY << wimp_ICON_BG_COLOUR_SHIFT);

    def.title_data.indirected_text.text = title_text;
    def.title_data.indirected_text.validation = NULL;
    def.title_data.indirected_text.size = (int)sizeof title_text;

    /* Clicks in the work area (there are no icons) arrive as ordinary
     * Select/Adjust/Menu click events. */
    def.work_flags = wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT;

    def.sprite_area = (osspriteop_area *)1;     /* the Wimp sprite pool */
    def.xmin = 400;
    def.ymin = 300;
    def.icon_count = 0;

    error = xwimp_create_window((wimp_window const *)&def, &win);
    if (error != NULL) {
        win = 0;
        ro_error("Could not create the main window: %s", error->errmess);
    }
}

void win_open(void)
{
    wimp_window_state st;

    if (win == 0)
        return;

    memset(&st, 0, sizeof st);
    st.w = win;
    if (xwimp_get_window_state(&st) != NULL)
        return;

    st.next = wimp_TOP;
    if (xwimp_open_window((wimp_open *)&st) != NULL)
        return;

    claim_caret();

    if (!opened) {
        opened = 1;
        hs_set_focus(app.client, ui_view_focus(view));
        hs_set_polling(app.client, 1, (unsigned long)os_read_monotonic_time());
    }

    win_model_changed(HS_R_ALL | HS_C_CONN);
}

void win_close(void)
{
    if (win == 0)
        return;

    xwimp_close_window(win);

    if (opened) {
        opened = 0;
        hs_set_polling(app.client, 0, 0);
        hs_set_focus(app.client, 0);
    }
}

void win_exit(void)
{
    ui_rows_free(&rows);
    ui_rows_free(&prev);
}

/* ------------------------------------------------------------------ */
/* Row layout (shared by drawing and hit-testing)                     */
/* ------------------------------------------------------------------ */

static int btn_width(const char *label)
{
    int w = (int)strlen(label) * CHAR_W + 32;

    return (w < BTN_MIN_W) ? BTN_MIN_W : w;
}

/* Fills in the horizontal extent of each button on a row. */
static void layout_buttons(const ui_row *r, int *x0, int *x1)
{
    int i;
    int x;

    if (r->kind == UR_TABS) {
        int w = 0;

        for (i = 0; i < r->nbtn; i++) {
            int bw = btn_width(r->btn[i].label);

            if (bw > w)
                w = bw;
        }

        x = PAD;
        for (i = 0; i < r->nbtn; i++) {
            x0[i] = x;
            x1[i] = x + w;
            x += w + BTN_GAP;
        }

        return;
    }

    /* Everything else keeps its buttons against the right-hand edge. */
    x = WORK_W - PAD;
    for (i = r->nbtn - 1; i >= 0; i--) {
        x1[i] = x;
        x0[i] = x - btn_width(r->btn[i].label);
        x = x0[i] - BTN_GAP;
    }
}

/* Where the values start in the run of label/value rows that `index` is in
 * (a heading or any other kind of row ends a run), so that a section of
 * short labels is not spread out by a long label somewhere else. */
static int value_x(int index)
{
    int first = index;
    int last = index;
    int longest = 0;
    int x;
    int i;

    while (first > 0 && rows.row[first - 1].kind == UR_KV)
        first--;
    while (last + 1 < rows.n && rows.row[last + 1].kind == UR_KV)
        last++;

    for (i = first; i <= last; i++) {
        int len = (int)strlen(rows.row[i].col[0]);

        if (len > longest)
            longest = len;
    }

    x = PAD + longest * CHAR_W + LABEL_GAP;
    if (x < LABEL_MIN_X)
        x = LABEL_MIN_X;
    if (x > LABEL_MAX_X)
        x = LABEL_MAX_X;

    return x;
}

/* Where text may run to before it would hit the first button. */
static int text_limit(const ui_row *r)
{
    int x0[UI_MAX_BTN];
    int x1[UI_MAX_BTN];

    if (r->nbtn == 0 || r->kind == UR_TABS)
        return WORK_W - PAD;

    layout_buttons(r, x0, x1);
    return x0[0] - 8;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static void plot_text(const char *text, int x0, int y0, int x1, int y1,
                      wimp_colour fg, wimp_colour bg, wimp_icon_flags flags)
{
    static char buf[160];
    wimp_icon ic;

    if (x1 <= x0)
        return;

    u_copy(buf, sizeof buf, text);

    memset(&ic, 0, sizeof ic);
    ic.extent.x0 = x0;
    ic.extent.y0 = y0;
    ic.extent.x1 = x1;
    ic.extent.y1 = y1;
    ic.flags = wimp_ICON_TEXT | wimp_ICON_INDIRECTED | wimp_ICON_VCENTRED |
               flags |
               ((wimp_icon_flags)fg << wimp_ICON_FG_COLOUR_SHIFT) |
               ((wimp_icon_flags)bg << wimp_ICON_BG_COLOUR_SHIFT);
    ic.data.indirected_text.text = buf;
    ic.data.indirected_text.validation = NULL;
    ic.data.indirected_text.size = (int)sizeof buf;

    xwimp_plot_icon(&ic);
}

/* Colours for a row style. Warnings and errors are drawn as filled bars so
 * they cannot be missed; "good" is just coloured text. */
static void style_colours(unsigned style, wimp_colour *fg, wimp_colour *bg,
                          wimp_icon_flags *fill)
{
    *fg = wimp_COLOUR_BLACK;
    *bg = wimp_COLOUR_VERY_LIGHT_GREY;
    *fill = 0;

    switch (style) {
        case US_GOOD:
            *fg = wimp_COLOUR_DARK_GREEN;
            break;
        case US_WARN:
            *fg = wimp_COLOUR_BLACK;
            *bg = wimp_COLOUR_ORANGE;
            *fill = wimp_ICON_FILLED;
            break;
        case US_BAD:
            *fg = wimp_COLOUR_WHITE;
            *bg = wimp_COLOUR_RED;
            *fill = wimp_ICON_FILLED;
            break;
        case US_DIM:
            *fg = wimp_COLOUR_DARK_GREY;
            break;
        case US_ACTIVE:
            *fg = wimp_COLOUR_WHITE;
            *bg = wimp_COLOUR_DARK_GREEN;
            *fill = wimp_ICON_FILLED;
            break;
        default:
            break;
    }
}

static void plot_button(const char *label, int x0, int y0, int x1, int y1,
                        int selected)
{
    plot_text(label, x0, y0, x1, y1, wimp_COLOUR_BLACK,
              wimp_COLOUR_LIGHT_GREY,
              wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_HCENTRED |
              (wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT) |
              (selected ? wimp_ICON_SELECTED : 0));
}

static void draw_row(const ui_row *r, int index)
{
    int top = -index * ROW_H;
    int y1 = top - CELL_INSET / 2;
    int y0 = top - ROW_H + CELL_INSET / 2;
    int by1 = top - CELL_INSET;
    int by0 = top - ROW_H + CELL_INSET;
    wimp_colour fg;
    wimp_colour bg;
    wimp_icon_flags fill;
    int bx0[UI_MAX_BTN];
    int bx1[UI_MAX_BTN];
    int limit = text_limit(r);
    int i;

    style_colours(r->style, &fg, &bg, &fill);

    switch (r->kind) {
        case UR_BLANK:
            return;

        case UR_HEAD:
            plot_text("", 0, y0, WORK_W, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_MID_LIGHT_GREY, wimp_ICON_FILLED);
            plot_text(r->col[0], PAD, y0, limit, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_MID_LIGHT_GREY, wimp_ICON_FILLED);
            break;

        case UR_NOTE:
            plot_text(r->col[0], PAD, y0, limit, y1, fg, bg, fill);
            break;

        case UR_KV: {
            int vx = value_x(index);

            plot_text(r->col[0], PAD, y0, vx, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_VERY_LIGHT_GREY, 0);
            plot_text(r->col[1], vx, y0, limit, y1, fg, bg, fill);
            break;
        }

        case UR_HEARD: {
            static const int edge[6] = { PAD, 148, 468, 588, 868, WORK_W };
            /* A live transmission highlights the whole row. */
            int c;

            for (c = 0; c < 5; c++)
                plot_text(r->col[c], edge[c], y0, edge[c + 1], y1, fg, bg,
                          fill);
            break;
        }

        case UR_BM:
            plot_text(r->col[0], PAD, y0, 160, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_VERY_LIGHT_GREY, 0);
            plot_text(r->col[1], 160, y0, 224, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_VERY_LIGHT_GREY, 0);
            plot_text(r->col[2], 224, y0, 640, y1, wimp_COLOUR_BLACK,
                      wimp_COLOUR_VERY_LIGHT_GREY, 0);
            plot_text(r->col[3], 640, y0, limit, y1, fg, bg, fill);
            break;

        case UR_TABS:
        default:
            break;
    }

    if (r->nbtn > 0) {
        layout_buttons(r, bx0, bx1);
        for (i = 0; i < r->nbtn; i++)
            plot_button(r->btn[i].label, bx0[i], by0, bx1[i], by1,
                        r->kind == UR_TABS && i == r->selected);
    }
}

void win_redraw(wimp_draw *d)
{
    osbool more;
    os_error *error;

    error = xwimp_redraw_window(d, &more);
    if (error != NULL)
        return;

    while (more) {
        /* The clip rectangle is in screen coordinates; the rows are laid
         * out from the top of the work area downwards. */
        int oy = d->box.y1 - d->yscroll;
        int first = (oy - d->clip.y1) / ROW_H;
        int last = (oy - d->clip.y0) / ROW_H;
        int i;

        if (first < 0)
            first = 0;
        if (last >= rows.n)
            last = rows.n - 1;

        for (i = first; i <= last; i++)
            draw_row(&rows.row[i], i);

        error = xwimp_get_rectangle(d, &more);
        if (error != NULL)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Keeping the display in step with the model                         */
/* ------------------------------------------------------------------ */

static int content_height(void)
{
    return rows.n * ROW_H + PAD;
}

/* The extent is the list's height, or the window's if that is taller: the
 * Wimp will not open a window bigger than its extent, so anything less would
 * stop the user enlarging it, and a short tab would shrink it for good. */
static void set_extent_for(int visible_h)
{
    os_box box;
    int h = content_height();

    if (visible_h > h)
        h = visible_h;

    /* Wimp_SetExtent also redraws the scroll bars and snaps the window back
     * on screen, and a drag of the scroll bar sends a stream of open
     * requests: only call it when the extent really changes. */
    if (h == extent_h)
        return;

    box.x0 = 0;
    box.y1 = 0;
    box.x1 = WORK_W;
    box.y0 = -h;

    if (xwimp_set_extent(win, &box) == NULL)
        extent_h = h;
}

static void apply_extent(void)
{
    wimp_window_state st;

    memset(&st, 0, sizeof st);
    st.w = win;
    if (xwimp_get_window_state(&st) != NULL) {
        set_extent_for(WIN_H);
        return;
    }

    set_extent_for(st.visible.y1 - st.visible.y0);

    /* A shorter list may leave the visible area past the end: re-open the
     * window so the Wimp pulls the scroll offsets back into range. */
    if (opened)
        xwimp_open_window((wimp_open *)&st);
}

void win_open_request(wimp_open *o)
{
    /* Make room for the size being asked for before the Wimp trims it. */
    set_extent_for(o->visible.y1 - o->visible.y0);
    xwimp_open_window(o);
}

static int ensure_prev(int n)
{
    ui_row *nr;

    if (prev.cap >= n)
        return 1;

    nr = (ui_row *)realloc(prev.row, (size_t)n * sizeof *nr);
    if (nr == NULL)
        return 0;

    prev.row = nr;
    prev.cap = n;
    return 1;
}

void win_model_changed(unsigned changed)
{
    int old_n;
    int n;
    int i;
    int first = -1;
    int last = -1;
    int use_prev;

    (void)changed;

    if (!opened || win == 0)
        return;

    /* Remember what is on screen so only the rows that differ are redrawn
     * (a full redraw every few seconds would visibly flicker). */
    use_prev = ensure_prev(rows.n);
    if (use_prev && rows.n > 0)
        memcpy(prev.row, rows.row, (size_t)rows.n * sizeof *rows.row);
    prev.n = use_prev ? rows.n : 0;
    old_n = rows.n;

    ui_rows_build(&rows, hs_get(app.client), hs_get_config(app.client), view);
    n = rows.n;

    if (!use_prev || n != old_n) {
        apply_extent();
    }

    if (!use_prev) {
        first = 0;
        last = (n > old_n ? n : old_n) - 1;
    } else {
        int top = (n > old_n) ? n : old_n;

        for (i = 0; i < top; i++) {
            int differs;

            if (i >= n || i >= old_n)
                differs = 1;
            else
                differs = memcmp(&rows.row[i], &prev.row[i],
                                 sizeof rows.row[i]) != 0;

            if (differs) {
                if (first < 0)
                    first = i;
                last = i;
            }
        }
    }

    if (first >= 0)
        xwimp_force_redraw(win, 0, -(last + 1) * ROW_H, WORK_W,
                           -first * ROW_H);
}

void win_set_view(ui_view v)
{
    wimp_window_state st;

    if (v == view)
        return;

    view = v;
    prev.n = 0;
    rows.n = 0;             /* force a complete redraw */

    if (opened) {
        /* The new tab's own data is fetched at once and then kept fresh. */
        hs_set_focus(app.client, ui_view_focus(view));

        memset(&st, 0, sizeof st);
        st.w = win;
        if (xwimp_get_window_state(&st) == NULL) {
            st.yscroll = 0;
            xwimp_open_window((wimp_open *)&st);
        }

        xwimp_force_redraw(win, 0, -4000, WORK_W, 0);
        win_model_changed(0);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

void win_click(const wimp_pointer *p)
{
    wimp_window_state st;
    int wx;
    int wy;
    int row;
    int i;
    int x0[UI_MAX_BTN];
    int x1[UI_MAX_BTN];

    /* A click here takes the keyboard, unless a dialogue is open: then it
     * stays in the dialogue's field, where the user was typing. */
    if (!dlg_is_open())
        claim_caret();

    if (p->buttons & wimp_CLICK_MENU) {
        menu_open_window(p);
        return;
    }

    if (!(p->buttons & (wimp_CLICK_SELECT | wimp_CLICK_ADJUST)))
        return;

    memset(&st, 0, sizeof st);
    st.w = win;
    if (xwimp_get_window_state(&st) != NULL)
        return;

    wx = p->pos.x - (st.visible.x0 - st.xscroll);
    wy = p->pos.y - (st.visible.y1 - st.yscroll);

    if (wy > 0)
        return;

    row = (-wy) / ROW_H;
    if (row < 0 || row >= rows.n)
        return;

    layout_buttons(&rows.row[row], x0, x1);

    for (i = 0; i < rows.row[row].nbtn; i++) {
        if (wx >= x0[i] && wx < x1[i]) {
            /* Copy it: running the action may rebuild (and move) the rows. */
            ui_btn b = rows.row[row].btn[i];

            act_run(&b);
            return;
        }
    }
}

void win_scroll(wimp_scroll *s)
{
    int vis_h = s->visible.y1 - s->visible.y0;
    int vis_w = s->visible.x1 - s->visible.x0;
    int ymax = 0;
    int ymin = -content_height() + vis_h;
    int xmax = WORK_W - vis_w;

    switch ((int)s->ymin) {
        case 1:  s->yscroll += ROW_H; break;
        case -1: s->yscroll -= ROW_H; break;
        case 2:  s->yscroll += vis_h - ROW_H; break;
        case -2: s->yscroll -= vis_h - ROW_H; break;
        case 3:  s->yscroll += ROW_H / 2; break;
        case -3: s->yscroll -= ROW_H / 2; break;
        default: break;
    }

    switch ((int)s->xmin) {
        case 1:  s->xscroll += 40; break;
        case -1: s->xscroll -= 40; break;
        case 2:  s->xscroll += vis_w - 40; break;
        case -2: s->xscroll -= vis_w - 40; break;
        default: break;
    }

    if (ymin > ymax)
        ymin = ymax;
    if (s->yscroll > ymax)
        s->yscroll = ymax;
    if (s->yscroll < ymin)
        s->yscroll = ymin;

    if (xmax < 0)
        xmax = 0;
    if (s->xscroll > xmax)
        s->xscroll = xmax;
    if (s->xscroll < 0)
        s->xscroll = 0;

    /* wimp_scroll starts with the same fields as wimp_open. */
    xwimp_open_window((wimp_open *)s);
}

void win_key(const wimp_key *k)
{
    if (k->c == wimp_KEY_F5) {
        hs_refresh(app.client, win_visible_parts());
        return;
    }

    xwimp_process_key(k->c);
}
