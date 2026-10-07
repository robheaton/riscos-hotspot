/*
 * fakewimp.h
 *
 * A fake RISC OS desktop for the host tests. The real front end (ro_*.c) is
 * compiled unchanged against the real OSLib headers; the handful of OSLib
 * functions it calls are provided here, backed by plain data, so a test can
 * run the whole program - icon bar icon, windows, redraw loop, dialogues,
 * menus - against the mock hotspot and inspect what it did.
 *
 * It models only what the front end relies on, following the real Wimp as
 * read from the RISC OS source (e.g. Wimp_PlotIcon adds the work area origin
 * to the icon rectangle; Wimp_OpenWindow clamps the scroll offsets; the Wimp
 * edits indirected writable icons in place and ends them with a CR). It
 * cannot tell us whether the real Wimp is happy with a flag combination,
 * only that the program's own logic holds together.
 */

#ifndef FAKEWIMP_H
#define FAKEWIMP_H

#include "oslib/os.h"
#include "oslib/wimp.h"

#define FW_MAX_WINDOWS  16
#define FW_MAX_PLOTS    1024
#define FW_MAX_ERRORS   32

typedef struct {
    int                used;
    int                deleted;
    wimp_w             w;
    int                open;
    wimp_window_base   def;
    wimp_window_state  state;
    os_box             extent;
    int                nicons;
    wimp_icon         *icons;
    int                redraw_queued;
} fw_window;

typedef struct {
    os_box        rel;          /* as given to Wimp_PlotIcon */
    os_box        abs;          /* with the work area origin added */
    unsigned      flags;
    char          text[160];
} fw_plot;

typedef struct {
    char                 msg[256];
    wimp_error_box_flags flags;
} fw_error;

/* ---- the script that plays the part of the user ------------------- */

/* Called whenever the program polls and nothing is queued. Queue events
 * with fw_queue() and return 1, or return 0 to end the session (the program
 * is then sent the desktop's Quit message). Returning 1 without queueing
 * gives the program a null event. */
typedef int (*fw_script_fn)(void *ud);
void fw_set_script(fw_script_fn fn, void *ud);
void fw_queue(wimp_event_no event, const wimp_block *block);

/* ---- inspecting the desktop --------------------------------------- */

fw_window *fw_window_by_handle(wimp_w w);
fw_window *fw_main_window(void);            /* the first window created */
fw_window *fw_top_dialog(void);             /* newest live window, if not main */
int fw_icon_count(const fw_window *win);

/* The rectangles drawn by the last complete redraw of the window. */
int fw_plot_count(void);
const fw_plot *fw_plot_at(int i);
/* Finds the nth plot whose text is exactly `text`; -1 if none. */
int fw_find_plot(const char *text, int nth);
/* Finds a plot with that text lying on the same row (y overlap) as the plot
 * `anchor`; -1 if none. */
int fw_find_plot_on_row(const char *text, int anchor);
/* Forces the window to be redrawn now (as the Wimp would after
 * Wimp_ForceRedraw) so fw_plot_* is current. */
void fw_redraw(wimp_w w);

/* Icon text helpers for dialogues. */
int fw_find_icon(const fw_window *win, const char *text);   /* -1 if none */
const char *fw_icon_text(const fw_window *win, int i);
void fw_type(fw_window *win, int i, const char *text);
int fw_icon_selected(const fw_window *win, int i);

/* ---- playing the user --------------------------------------------- */

/* Click at a screen position in window `w` (icon -1 for the work area). */
void fw_click(wimp_w w, wimp_i i, int x, int y, int buttons);
/* Click the centre of a plot from the last redraw. */
void fw_click_plot(int plot_index, int buttons);
/* Click an icon in a dialogue (radio icons toggle, like the Wimp). */
void fw_click_icon(fw_window *win, int i, int buttons);
void fw_key(wimp_w w, wimp_i i, int key);
void fw_menu_select(const int *items);       /* -1 terminated */
void fw_message_quit(void);

/* ---- observations ------------------------------------------------- */

extern int          fw_bar_created;
extern wimp_icon    fw_bar_icon;
extern wimp_menu   *fw_menu;
extern int          fw_menu_x, fw_menu_y;
extern int          fw_menu_count;
extern wimp_pointer fw_pointer;
extern int          fw_error_count;
extern fw_error     fw_errors[FW_MAX_ERRORS];
extern int          fw_next_click;      /* what Wimp_ReportError "returns" */
extern int          fw_caret_icon;
extern wimp_w       fw_caret_window;    /* who the keyboard goes to (0: nobody) */
extern int          fw_keys_lost;       /* keys sent with no caret owner */
extern int          fw_api_violations;  /* calls the real Wimp would mishandle */
extern int          fw_set_extent_count;/* Wimp_SetExtent calls so far */
extern int          fw_process_key_count;
extern int          fw_poll_count;

/* The Wimp opens a window used as an entry's submenu when the pointer moves
 * onto the entry's arrow; this plays that part. NULL if the entry has none. */
fw_window *fw_menu_open_submenu_window(int item);

/* The menu item text / index search. */
int fw_menu_item_count(const wimp_menu *m);
const char *fw_menu_item_text(const wimp_menu *m, int i);
int fw_menu_find(const wimp_menu *m, const char *text);

void fw_reset(void);

#endif /* FAKEWIMP_H */
