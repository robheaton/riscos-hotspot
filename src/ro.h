/*
 * ro.h
 *
 * Declarations shared by the RISC OS front end (the ro_*.c files). The
 * front end is a Wimp task built directly on OSLib, in the same style as
 * the other native projects here: windows are built in code rather than
 * loaded from a Templates file.
 *
 *   ro_info.c     the Program information window
 *   ro_main.c     start-up, the poll loop, the icon bar icon, messages
 *   ro_win.c      the main window: a scrolling list of rows, drawn by hand
 *   ro_dlg.c      one reusable dialogue window (Choices, link prompts...)
 *   ro_menu.c     the icon bar menu and the main window's menu
 *   ro_act.c      what every button and menu item does
 *   ro_choices.c  the Choices file and the diagnostics report
 */

#ifndef HS_RO_H
#define HS_RO_H

#include <stddef.h>

#include "oslib/os.h"
#include "oslib/wimp.h"

#include "client.h"
#include "rows.h"

#define APP_NAME     "Hotspot"
#define APP_VERSION  "0.05"

/* What the Program information window says. \251 is the copyright sign in
 * the Latin-1 alphabet RISC OS uses. */
#define APP_PURPOSE  "WPSD hotspot controller"
#define APP_AUTHOR   "\251 Rob Heaton, 2026"

typedef struct {
    hs_client *client;
    hs_config  cfg;         /* settings as saved in Choices */
    wimp_t     task;
    wimp_i     bar_icon;
    int        quit;
} app_state;

extern app_state app;

/* ---- ro_main.c ---------------------------------------------------- */

void ro_error(const char *fmt, ...);
void ro_info(const char *fmt, ...);
int  ro_confirm(const char *fmt, ...);          /* 1 if OK was clicked */
void ro_apply_config(const hs_config *cfg);     /* use + save new settings */
void ro_open_at_pointer(wimp_w w, int width, int height);

/* ---- ro_win.c ----------------------------------------------------- */

void win_init(void);
wimp_w win_handle(void);
int  win_is_open(void);
void win_open(void);
void win_close(void);
void win_exit(void);           /* free everything at the end */
void win_model_changed(unsigned changed);
void win_set_view(ui_view v);
ui_view win_view(void);
/* Give the main window the keyboard (an invisible caret), so F5 reaches it. */
void win_claim_caret(void);
/* What a refresh should cover: the radio and status, plus whatever the
 * visible tab shows (HS_R_*). */
unsigned win_visible_parts(void);
void win_open_request(wimp_open *o);   /* the user moved or resized it */
void win_redraw(wimp_draw *d);
void win_click(const wimp_pointer *p);
void win_scroll(wimp_scroll *s);
void win_key(const wimp_key *k);

/* ---- ro_dlg.c ----------------------------------------------------- */

#define DLG_MAX_FIELDS  6
#define DLG_MAX_CHECKS  3
#define DLG_BUF         100

typedef struct {
    const char *label;
    const char *initial;
    int         size;           /* buffer size incl. terminator (<= DLG_BUF) */
    const char *validation;     /* Wimp validation string, or NULL */
    int         password;
} dlg_field;

typedef struct {
    const char *label;
    int         initial;
} dlg_check;

typedef void (*dlg_ok_fn)(char values[][DLG_BUF], const int *checks,
                          void *ud);

typedef struct {
    const char *title;
    const char *text;           /* intro lines separated by '\n', or NULL */
    int         nfields;
    dlg_field   field[DLG_MAX_FIELDS];
    int         nchecks;
    dlg_check   check[DLG_MAX_CHECKS];
    const char *ok_label;       /* NULL = "OK"; "" = no OK button */
    dlg_ok_fn   on_ok;
    void       *ud;
} dlg_spec;

void dlg_open(const dlg_spec *spec);
void dlg_close(void);
int  dlg_is_open(void);
int  dlg_owns(wimp_w w);
void dlg_click(const wimp_pointer *p);
void dlg_key(const wimp_key *k);

/* ---- ro_info.c ---------------------------------------------------- */

/* The "Program information" window: created once, never opened by us - the
 * icon bar menu's Info entry has it as its submenu, and the Wimp shows it
 * (and takes it away with the menu) as it does for every RISC OS program. */
void info_init(void);
wimp_w info_window(void);               /* 0 if it could not be created */
void info_exit(void);

/* ---- ro_menu.c ---------------------------------------------------- */

void menu_open_iconbar(const wimp_pointer *p);
void menu_open_window(const wimp_pointer *p);
void menu_selection(const wimp_selection *sel);
void menu_deleted(void);
void menu_exit(void);

/* ---- ro_act.c ----------------------------------------------------- */

/* Carry out a button or menu action. */
void act_run(const ui_btn *b);
void act_choices(void);
void act_find(void);
void act_save_diagnostics(void);
/* The client reports that an action finished / that the search for a hotspot
 * has an outcome. */
void act_action_finished(int ok);
void act_scan_results(void);

/* ---- ro_choices.c ------------------------------------------------- */

/* Fills in `cfg` from the shipped defaults and the user's own file. Returns
 * 1 if the user has saved choices before, 0 if this is the first run. */
int choices_load(hs_config *cfg);
/* Returns NULL on success or a message describing what went wrong. */
const char *choices_save(const hs_config *cfg);
/* Writes a diagnostics report; on success returns NULL and the path used. */
const char *diag_save(const hs_client *hs, char *path_out, size_t cap);

#endif /* HS_RO_H */
