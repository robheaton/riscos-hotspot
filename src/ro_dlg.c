/*
 * ro_dlg.c
 *
 * One reusable dialogue window. A caller describes what it needs (a title,
 * a few lines of text, up to six labelled writable fields, up to three
 * option buttons) in a dlg_spec; the window is built in code, shown near
 * the pointer, and deleted again when it is closed. Only one dialogue is
 * ever open, which keeps the state trivial: opening another one closes the
 * first.
 *
 * Everything the Wimp keeps pointers to (icon text, validation strings,
 * the title) lives in the static `D` below for as long as the window does.
 */

#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define DLG_WIDTH       960
#define DLG_MARGIN      24
#define DLG_LINE_H      40
#define DLG_LABEL_H     36
#define DLG_FIELD_H     52
#define DLG_BTN_W       170
#define DLG_BTN_H       52
#define DLG_GAP         14
#define DLG_MAX_TEXT    8
#define DLG_TEXT_LEN    100

typedef struct {
    int         open;
    wimp_w      w;
    int         width;
    int         height;

    char        title[48];
    char        text[DLG_MAX_TEXT][DLG_TEXT_LEN];
    int         ntext;
    char        label[DLG_MAX_FIELDS][80];
    char        check_label[DLG_MAX_CHECKS][80];
    char        buf[DLG_MAX_FIELDS][DLG_BUF];
    int         bufsize[DLG_MAX_FIELDS];
    char        valid[DLG_MAX_FIELDS][48];
    char        ok_label[24];
    char        cancel_label[24];
    char        check_valid[DLG_MAX_CHECKS][24];

    int         nfields;
    int         nchecks;
    wimp_i      field_icon[DLG_MAX_FIELDS];
    wimp_i      check_icon[DLG_MAX_CHECKS];
    wimp_i      ok_icon;
    wimp_i      cancel_icon;
    int         has_ok;

    dlg_ok_fn   on_ok;
    void       *ud;
} dlg_state;

static dlg_state D;

/* ------------------------------------------------------------------ */
/* Icon construction                                                  */
/* ------------------------------------------------------------------ */

static wimp_icon_flags colours(wimp_colour fg, wimp_colour bg)
{
    return ((wimp_icon_flags)fg << wimp_ICON_FG_COLOUR_SHIFT) |
           ((wimp_icon_flags)bg << wimp_ICON_BG_COLOUR_SHIFT);
}

static void set_icon(wimp_icon *ic, int x0, int y0, int x1, int y1,
                     wimp_icon_flags flags, char *text, int size,
                     char *validation)
{
    memset(ic, 0, sizeof *ic);
    ic->extent.x0 = x0;
    ic->extent.y0 = y0;
    ic->extent.x1 = x1;
    ic->extent.y1 = y1;
    ic->flags = flags | wimp_ICON_TEXT | wimp_ICON_INDIRECTED;
    ic->data.indirected_text.text = text;
    ic->data.indirected_text.validation = validation;
    ic->data.indirected_text.size = size;
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                */
/* ------------------------------------------------------------------ */

int dlg_is_open(void)
{
    return D.open;
}

int dlg_owns(wimp_w w)
{
    return D.open && w == D.w;
}

void dlg_close(void)
{
    if (!D.open)
        return;

    xwimp_close_window(D.w);
    xwimp_delete_window(D.w);
    D.open = 0;

    /* Deleting the window leaves nobody with the keyboard: give it back to
     * the main window, so F5 works without having to click there first. */
    win_claim_caret();
}

static void copy_lines(const char *text)
{
    const char *p = text;

    D.ntext = 0;
    while (p != NULL && *p != '\0' && D.ntext < DLG_MAX_TEXT) {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);

        u_copyn(D.text[D.ntext], sizeof D.text[D.ntext], p, len);
        D.ntext++;
        p = (nl != NULL) ? nl + 1 : NULL;
    }
}

void dlg_open(const dlg_spec *spec)
{
    wimp_window_base *def;
    wimp_icon *icons;
    size_t bytes;
    int nicons;
    int ic = 0;
    int y;
    int i;
    int btn_y1;
    int btn_y0;
    os_error *error;

    dlg_close();
    memset(&D, 0, sizeof D);

    u_copy(D.title, sizeof D.title, spec->title);
    copy_lines(spec->text);

    D.nfields = (spec->nfields > DLG_MAX_FIELDS) ? DLG_MAX_FIELDS
                                                 : spec->nfields;
    D.nchecks = (spec->nchecks > DLG_MAX_CHECKS) ? DLG_MAX_CHECKS
                                                 : spec->nchecks;
    D.on_ok = spec->on_ok;
    D.ud = spec->ud;
    D.has_ok = !(spec->ok_label != NULL && spec->ok_label[0] == '\0');

    u_copy(D.ok_label, sizeof D.ok_label,
           (spec->ok_label != NULL && spec->ok_label[0] != '\0')
               ? spec->ok_label : "OK");
    u_copy(D.cancel_label, sizeof D.cancel_label,
           D.has_ok ? "Cancel" : "Close");

    for (i = 0; i < D.nfields; i++) {
        const dlg_field *f = &spec->field[i];

        u_copy(D.label[i], sizeof D.label[i], f->label);
        if (f->initial != NULL)
            u_copy(D.buf[i], sizeof D.buf[i], f->initial);
        D.bufsize[i] = (f->size > 1 && f->size <= DLG_BUF) ? f->size
                                                           : DLG_BUF;
        D.buf[i][D.bufsize[i] - 1] = '\0';

        /* "Ktar": Tab, the arrow keys and Return move from one field to the
         * next, and Return in the last one is handed back to us (dlg_key),
         * as in the ROM's own dialogues. */
        if (f->password && f->validation != NULL)
            snprintf(D.valid[i], sizeof D.valid[i], "Ktar;Pptr_write;D*;%s",
                     f->validation);
        else if (f->password)
            u_copy(D.valid[i], sizeof D.valid[i], "Ktar;Pptr_write;D*");
        else if (f->validation != NULL)
            snprintf(D.valid[i], sizeof D.valid[i], "Ktar;Pptr_write;%s",
                     f->validation);
        else
            u_copy(D.valid[i], sizeof D.valid[i], "Ktar;Pptr_write");
    }

    for (i = 0; i < D.nchecks; i++) {
        u_copy(D.check_label[i], sizeof D.check_label[i],
               spec->check[i].label);
        u_copy(D.check_valid[i], sizeof D.check_valid[i], "Soptoff,opton");
    }

    D.width = DLG_WIDTH;

    nicons = D.ntext + D.nfields * 2 + D.nchecks + (D.has_ok ? 2 : 1);
    bytes = sizeof(wimp_window_base) + (size_t)nicons * sizeof(wimp_icon);
    def = (wimp_window_base *)calloc(1, bytes);
    if (def == NULL) {
        ro_error("Out of memory.");
        return;
    }

    icons = (wimp_icon *)((char *)def + sizeof(wimp_window_base));

    y = -DLG_MARGIN;

    for (i = 0; i < D.ntext; i++) {
        set_icon(&icons[ic++], DLG_MARGIN, y - DLG_LINE_H,
                 D.width - DLG_MARGIN, y,
                 wimp_ICON_VCENTRED |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_VERY_LIGHT_GREY),
                 D.text[i], DLG_TEXT_LEN, NULL);
        y -= DLG_LINE_H;
    }

    if (D.ntext > 0)
        y -= DLG_GAP;

    for (i = 0; i < D.nfields; i++) {
        set_icon(&icons[ic++], DLG_MARGIN, y - DLG_LABEL_H,
                 D.width - DLG_MARGIN, y,
                 wimp_ICON_VCENTRED |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_VERY_LIGHT_GREY),
                 D.label[i], (int)sizeof D.label[i], NULL);
        y -= DLG_LABEL_H;

        D.field_icon[i] = (wimp_i)ic;
        set_icon(&icons[ic++], DLG_MARGIN, y - DLG_FIELD_H,
                 D.width - DLG_MARGIN, y,
                 wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_VCENTRED |
                 (wimp_BUTTON_WRITABLE << wimp_ICON_BUTTON_TYPE_SHIFT) |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_WHITE),
                 D.buf[i], D.bufsize[i], D.valid[i]);
        y -= DLG_FIELD_H + DLG_GAP;
    }

    for (i = 0; i < D.nchecks; i++) {
        D.check_icon[i] = (wimp_i)ic;
        set_icon(&icons[ic], DLG_MARGIN, y - DLG_LINE_H,
                 D.width - DLG_MARGIN, y,
                 wimp_ICON_SPRITE | wimp_ICON_VCENTRED |
                 (wimp_BUTTON_RADIO << wimp_ICON_BUTTON_TYPE_SHIFT) |
                 (spec->check[i].initial ? wimp_ICON_SELECTED : 0) |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_VERY_LIGHT_GREY),
                 D.check_label[i], (int)sizeof D.check_label[i],
                 D.check_valid[i]);
        ic++;
        y -= DLG_LINE_H;
    }

    y -= DLG_GAP;
    btn_y1 = y;
    btn_y0 = y - DLG_BTN_H;

    D.cancel_icon = (wimp_i)ic;
    if (D.has_ok) {
        set_icon(&icons[ic++], DLG_MARGIN, btn_y0, DLG_MARGIN + DLG_BTN_W,
                 btn_y1,
                 wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_HCENTRED |
                 wimp_ICON_VCENTRED |
                 (wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT) |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_LIGHT_GREY),
                 D.cancel_label, (int)sizeof D.cancel_label, NULL);

        D.ok_icon = (wimp_i)ic;
        set_icon(&icons[ic++], D.width - DLG_MARGIN - DLG_BTN_W, btn_y0,
                 D.width - DLG_MARGIN, btn_y1,
                 wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_HCENTRED |
                 wimp_ICON_VCENTRED |
                 (wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT) |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_LIGHT_GREY),
                 D.ok_label, (int)sizeof D.ok_label, NULL);
    } else {
        set_icon(&icons[ic++], D.width - DLG_MARGIN - DLG_BTN_W, btn_y0,
                 D.width - DLG_MARGIN, btn_y1,
                 wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_HCENTRED |
                 wimp_ICON_VCENTRED |
                 (wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT) |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_LIGHT_GREY),
                 D.cancel_label, (int)sizeof D.cancel_label, NULL);
    }

    D.height = -(btn_y0 - DLG_MARGIN);

    def->visible.x0 = 300;
    def->visible.y0 = 300;
    def->visible.x1 = 300 + D.width;
    def->visible.y1 = 300 + D.height;
    def->xscroll = 0;
    def->yscroll = 0;
    def->next = wimp_TOP;
    def->flags = wimp_WINDOW_NEW_FORMAT | wimp_WINDOW_MOVEABLE |
                 wimp_WINDOW_AUTO_REDRAW | wimp_WINDOW_BACK_ICON |
                 wimp_WINDOW_CLOSE_ICON | wimp_WINDOW_TITLE_ICON;
    def->title_fg = wimp_COLOUR_BLACK;
    def->title_bg = wimp_COLOUR_LIGHT_GREY;
    def->work_fg = wimp_COLOUR_BLACK;
    def->work_bg = wimp_COLOUR_VERY_LIGHT_GREY;
    def->scroll_outer = wimp_COLOUR_MID_LIGHT_GREY;
    def->scroll_inner = wimp_COLOUR_VERY_LIGHT_GREY;
    def->highlight_bg = wimp_COLOUR_CREAM;
    def->extent.x0 = 0;
    def->extent.y0 = -D.height;
    def->extent.x1 = D.width;
    def->extent.y1 = 0;
    def->title_flags = wimp_ICON_TEXT | wimp_ICON_INDIRECTED |
                       wimp_ICON_BORDER | wimp_ICON_HCENTRED |
                       wimp_ICON_VCENTRED | wimp_ICON_FILLED |
                       colours(wimp_COLOUR_BLACK, wimp_COLOUR_LIGHT_GREY);
    def->title_data.indirected_text.text = D.title;
    def->title_data.indirected_text.validation = NULL;
    def->title_data.indirected_text.size = (int)sizeof D.title;
    def->work_flags = wimp_BUTTON_NEVER << wimp_ICON_BUTTON_TYPE_SHIFT;
    def->sprite_area = (osspriteop_area *)1;
    def->xmin = 0;
    def->ymin = 0;
    def->icon_count = nicons;

    error = xwimp_create_window((wimp_window const *)def, &D.w);
    free(def);

    if (error != NULL) {
        ro_error("Could not create the dialogue: %s", error->errmess);
        return;
    }

    D.open = 1;
    ro_open_at_pointer(D.w, D.width, D.height);

    if (D.nfields > 0)
        xwimp_set_caret_position(D.w, D.field_icon[0], 0, 0, -1,
                                 (int)strlen(D.buf[0]));
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

/* The Wimp edits indirected icon text in place and ends it with a control
 * character (CR), not necessarily a NUL. */
static void field_text(int i, char *out)
{
    const char *p = D.buf[i];
    int n = 0;

    while (n < D.bufsize[i] - 1 && (unsigned char)p[n] >= 32) {
        out[n] = p[n];
        n++;
    }

    out[n] = '\0';
    u_trim(out);
}

static void submit(int keep_open)
{
    char values[DLG_MAX_FIELDS][DLG_BUF];
    int checks[DLG_MAX_CHECKS];
    dlg_ok_fn fn = D.on_ok;
    void *ud = D.ud;
    int i;

    for (i = 0; i < D.nfields; i++)
        field_text(i, values[i]);

    for (i = 0; i < D.nchecks; i++) {
        wimp_icon_state st;

        memset(&st, 0, sizeof st);
        st.w = D.w;
        st.i = D.check_icon[i];
        checks[i] = (xwimp_get_icon_state(&st) == NULL) &&
                    (st.icon.flags & wimp_ICON_SELECTED) != 0;
    }

    /* The callback may open another dialogue, which would rebuild D, so the
     * window is closed first and everything needed has been copied out. */
    if (!keep_open)
        dlg_close();

    if (fn != NULL)
        fn(values, checks, ud);
}

void dlg_click(const wimp_pointer *p)
{
    if (!D.open || p->w != D.w)
        return;

    if (!(p->buttons & (wimp_CLICK_SELECT | wimp_CLICK_ADJUST)))
        return;

    if (p->i == D.cancel_icon) {
        if (p->buttons & wimp_CLICK_SELECT)
            dlg_close();
        else if (!D.has_ok)
            dlg_close();
        return;
    }

    if (D.has_ok && p->i == D.ok_icon)
        submit((p->buttons & wimp_CLICK_ADJUST) != 0);
}

void dlg_key(const wimp_key *k)
{
    if (!D.open || k->w != D.w) {
        xwimp_process_key(k->c);
        return;
    }

    if (k->c == wimp_KEY_RETURN) {
        if (D.has_ok)
            submit(0);
        else
            dlg_close();
        return;
    }

    if (k->c == wimp_KEY_ESCAPE) {
        dlg_close();
        return;
    }

    xwimp_process_key(k->c);
}
