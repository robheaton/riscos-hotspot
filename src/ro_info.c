/*
 * ro_info.c
 *
 * The "Program information" window, the way RISC OS programs have always
 * done it: the first entry of the icon bar menu is "Info" with an arrow, and
 * the window opens beside it as that entry's submenu when the pointer moves
 * onto the arrow, going away again with the menu. Nothing here opens it: the
 * window is created once at start-up and its handle is simply put in the
 * menu entry's submenu field (ro_menu.c); the Wimp does the rest.
 *
 * The layout is the ROM's (Edit, Paint, Draw and Chars all have the same
 * window): the title "Program information", then Name, Purpose, Author and
 * Version, each a right-justified label and an inset, centred, read-only
 * field (validation "R2" draws the inset border).
 */

#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INFO_W        640
#define INFO_H        248
#define INFO_ROW      60        /* distance from one field to the next */
#define INFO_FIELD_H  52
#define INFO_LABEL_X0 14
#define INFO_LABEL_X1 150
#define INFO_FIELD_X0 154
#define INFO_FIELD_X1 (INFO_W - 10)

#define INFO_ROWS     4

static wimp_w info_w;

/* Everything the Wimp points at has to stay put as long as the window does. */
static char title[24] = "Program information";
static char label[INFO_ROWS][12] = { "Name", "Purpose", "Author", "Version" };
static char name[24] = APP_NAME;
static char purpose[40] = APP_PURPOSE;
static char author[40] = APP_AUTHOR;
static char version[32];
static char border[8] = "R2";

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

/* "0.03 (04 Oct 2026)": the version and the date it was built, as the
 * Style Guide has them. __DATE__ is "Oct  4 2026". */
static void make_version(char *out, size_t cap)
{
    const char *d = __DATE__;

    snprintf(out, cap, "%s (%02d %.3s %.4s)", APP_VERSION, atoi(d + 4), d,
             d + 7);
}

wimp_w info_window(void)
{
    return info_w;
}

void info_init(void)
{
    char *value[INFO_ROWS] = { name, purpose, author, version };
    int vsize[INFO_ROWS] = { (int)sizeof name, (int)sizeof purpose,
                             (int)sizeof author, (int)sizeof version };
    wimp_window_base *def;
    wimp_icon *icons;
    size_t bytes = sizeof(wimp_window_base) + 2 * INFO_ROWS * sizeof(wimp_icon);
    os_error *error;
    int i;

    make_version(version, sizeof version);

    def = (wimp_window_base *)calloc(1, bytes);
    if (def == NULL) {
        ro_error("Out of memory.");
        return;
    }

    icons = (wimp_icon *)((char *)def + sizeof(wimp_window_base));

    for (i = 0; i < INFO_ROWS; i++) {
        int y1 = -8 - i * INFO_ROW;
        int y0 = y1 - INFO_FIELD_H;

        set_icon(&icons[2 * i], INFO_LABEL_X0, y0, INFO_LABEL_X1, y1,
                 wimp_ICON_VCENTRED | wimp_ICON_RJUSTIFIED |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_VERY_LIGHT_GREY),
                 label[i], (int)sizeof label[i], NULL);

        set_icon(&icons[2 * i + 1], INFO_FIELD_X0, y0, INFO_FIELD_X1, y1,
                 wimp_ICON_BORDER | wimp_ICON_FILLED | wimp_ICON_HCENTRED |
                 wimp_ICON_VCENTRED |
                 colours(wimp_COLOUR_BLACK, wimp_COLOUR_VERY_LIGHT_GREY),
                 value[i], vsize[i], border);
    }

    /* Where it first sits does not matter: it is placed by the Wimp when it
     * opens it as a submenu. The flags are the ROM program information
     * windows': a title bar and nothing else, moveable, redrawn by the Wimp. */
    def->visible.x0 = 400;
    def->visible.y0 = 500;
    def->visible.x1 = 400 + INFO_W;
    def->visible.y1 = 500 + INFO_H;
    def->xscroll = 0;
    def->yscroll = 0;
    def->next = wimp_TOP;
    def->flags = wimp_WINDOW_NEW_FORMAT | wimp_WINDOW_MOVEABLE |
                 wimp_WINDOW_AUTO_REDRAW | wimp_WINDOW_TITLE_ICON;
    def->title_fg = wimp_COLOUR_BLACK;
    def->title_bg = wimp_COLOUR_LIGHT_GREY;
    def->work_fg = wimp_COLOUR_BLACK;
    def->work_bg = wimp_COLOUR_VERY_LIGHT_GREY;
    def->scroll_outer = wimp_COLOUR_MID_LIGHT_GREY;
    def->scroll_inner = wimp_COLOUR_VERY_LIGHT_GREY;
    def->highlight_bg = wimp_COLOUR_CREAM;
    def->extent.x0 = 0;
    def->extent.y0 = -INFO_H;
    def->extent.x1 = INFO_W;
    def->extent.y1 = 0;
    def->title_flags = wimp_ICON_TEXT | wimp_ICON_INDIRECTED |
                       wimp_ICON_BORDER | wimp_ICON_HCENTRED |
                       wimp_ICON_VCENTRED | wimp_ICON_FILLED |
                       colours(wimp_COLOUR_BLACK, wimp_COLOUR_LIGHT_GREY);
    def->title_data.indirected_text.text = title;
    def->title_data.indirected_text.validation = NULL;
    def->title_data.indirected_text.size = (int)sizeof title;
    def->work_flags = wimp_BUTTON_NEVER << wimp_ICON_BUTTON_TYPE_SHIFT;
    def->sprite_area = (osspriteop_area *)1;
    def->xmin = 0;
    def->ymin = 0;
    def->icon_count = 2 * INFO_ROWS;

    error = xwimp_create_window((wimp_window const *)def, &info_w);
    free(def);

    if (error != NULL) {
        info_w = 0;
        ro_error("Could not create the program information window: %s",
                 error->errmess);
    }
}

void info_exit(void)
{
    if (info_w != 0) {
        xwimp_delete_window(info_w);
        info_w = 0;
    }
}
