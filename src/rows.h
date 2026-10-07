/*
 * rows.h
 *
 * Turns the client's model into the list of display rows the main window
 * draws. Kept free of any Wimp code so the layout logic can be run and
 * inspected on the dev machine (the host tests print these rows).
 *
 * The window is one scrolling list of rows. A row has up to five text
 * columns and up to five buttons; what the columns mean depends on the row
 * kind and is fixed by the renderer (ro_win.c).
 */

#ifndef HS_ROWS_H
#define HS_ROWS_H

#include "client.h"

typedef enum {
    VIEW_STATUS = 0,
    VIEW_HEARD,
    VIEW_DMR,           /* DMR networks, BrandMeister, TGIF */
    VIEW_LINKS,         /* YSF, D-Star, P25, NXDN reflectors and talkgroups */
    VIEW_SYSTEM,
    VIEW_COUNT
} ui_view;

typedef enum {
    UR_BLANK = 0,
    UR_HEAD,        /* col0 = title (a bar); buttons on the right */
    UR_NOTE,        /* col0 = one wide line of text */
    UR_KV,          /* col0 = label, col1 = value; buttons on the right */
    UR_HEARD,       /* col0 time, col1 call, col2 mode, col3 target, col4 info */
    UR_BM,          /* col0 TG, col1 slot, col2 name, col3 state/timeout */
    UR_TABS         /* buttons only; the current view's button is selected */
} ui_rowkind;

typedef enum {
    US_NORMAL = 0,
    US_GOOD,
    US_WARN,
    US_BAD,
    US_DIM,
    US_ACTIVE       /* live transmission: stands out */
} ui_style;

typedef enum {
    UA_NONE = 0,
    UA_REFRESH,
    UA_CHOICES,
    UA_FIND,            /* search the network for the hotspot */
    UA_FIND_STOP,
    UA_VIEW,            /* a[0] = ui_view */
    UA_MODE,            /* arg = mode name, a[0] = 1 pause / 0 resume */
    UA_DMRNET,          /* arg = net id, a[0] = 1 enable / 0 disable */
    UA_BM_LINK,         /* a[0] = tg, a[1] = slot, a[2] = 1 link / 0 drop */
    UA_BM_DELETE,       /* a[0] = tg, a[1] = slot */
    UA_BM_DROP_QSO,     /* a[1] = slot */
    UA_BM_DROP_DYN,     /* a[1] = slot */
    UA_BM_ADD,
    UA_LINK,            /* a[0] = ui_proto, a[1] = 1 link / 0 unlink,
                         * a[2] = TGIF timeslot (0 = ask / default) */
    UA_LINK_TO,         /* a[0] = ui_proto, arg = a recently used target */
    UA_SYS,             /* arg = system_api action, label = what to confirm */
    UA_DIAG,
    UA_QUIT
} ui_action;

typedef enum {
    PROTO_YSF = 0,
    PROTO_P25,
    PROTO_NXDN,
    PROTO_DSTAR,
    PROTO_TGIF
} ui_proto;

typedef struct {
    unsigned char action;
    int           a[3];
    char          arg[24];
    char          label[24];
} ui_btn;

#define UI_MAX_BTN 5
#define UI_MAX_ROWS 400     /* a safety cap on any one list */

/* A value longer than this (a hardware description, say) is carried on in
 * rows below it rather than running off the edge of the window. */
#define UI_VALUE_CHARS 40

typedef struct {
    unsigned char kind;
    unsigned char style;
    unsigned char selected;     /* UR_TABS: which button is the current view */
    unsigned char nbtn;
    char          col[5][112];
    ui_btn        btn[UI_MAX_BTN];
} ui_row;

typedef struct {
    ui_row *row;
    int     n;
    int     cap;
} ui_rows;

void ui_rows_init(ui_rows *r);
void ui_rows_free(ui_rows *r);

/* Rebuild the rows for `view` from the current model. */
void ui_rows_build(ui_rows *r, const hs_model *m, const hs_config *cfg,
                   ui_view view);

/* Helpers the UI also uses when building menus and dialogs. */
int ui_mode_state(const hs_model *m, const char *mode);     /* PILL_* or -1 */
const char *ui_pill_value(const hs_model *m, const char *label);
const char *ui_view_name(ui_view v);

/* What the tab for `view` needs the client to keep fresh (HS_R_*). */
unsigned ui_view_focus(ui_view v);

/* The timeslots the DMR status page says are in use: bit 0 = slot 1, bit 1 =
 * slot 2. Both when it does not say. */
unsigned ui_dmr_slots(const hs_model *m);

/* The hotspot's own idea of what `mode` ("YSF", "D-Star", "P25", "NXDN") is
 * linked to - the value of the "Link" pill in that mode's section - or NULL
 * if the status page has no such pill. */
const char *ui_current_link(const hs_model *m, const char *mode);

/* Text for the button of a remembered target (the TGIF ones are stored as
 * "2341/2": talkgroup and timeslot). */
void ui_recent_label(ui_proto proto, const char *target, char *out,
                     size_t cap);

#endif /* HS_ROWS_H */
