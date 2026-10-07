/*
 * client.h
 *
 * The hotspot client: owns the connection settings, a small queue of HTTP
 * jobs, the polling schedule and the most recently parsed state of the
 * hotspot (the "model"). It has no Wimp code in it - the desktop front end
 * just calls hs_step() from its poll loop and redraws when it reports that
 * something changed - which is also what lets the whole thing run against a
 * mock WPSD server on the Linux dev machine.
 *
 * One HTTP request is in flight at a time. Polling fetches, in rotation:
 *   every cycle      radio state, last heard
 *   every 3rd cycle  mode/network status pills
 *   every 6th cycle  system cards, BrandMeister talkgroups
 */

#ifndef HS_CLIENT_H
#define HS_CLIENT_H

#include <stddef.h>
#include <stdio.h>

#include "http.h"
#include "wpsd.h"

/* The targets last linked to, newest first, so a reflector or talkgroup can
 * be selected again with one click: one list per protocol (see rows.h:
 * YSF, P25, NXDN, D-Star, TGIF). */
#define HS_MRU_PROTOS   5
#define HS_MRU_N        3
#define HS_MRU_LEN      24

typedef struct {
    char host[96];
    int  port;
    char user[48];
    char pass[48];
    int  refresh_s;     /* poll interval in seconds */
    int  heard_rows;    /* last heard rows to fetch */
    int  show_bm;       /* read the BrandMeister manager page */
    int  names;         /* ask for operator names (slower on the hotspot) */
    char recent[HS_MRU_PROTOS][HS_MRU_N][HS_MRU_LEN];
} hs_config;

void hs_config_defaults(hs_config *cfg);

/* Remember `target` as the newest of the recent targets for protocol `proto`
 * (a ui_proto value): an entry already there moves to the front, otherwise
 * the oldest drops off. */
void hs_mru_push(hs_config *cfg, int proto, const char *target);

/* What a refresh covers / what changed. */
#define HS_R_RADIO   0x01u
#define HS_R_HEARD   0x02u
#define HS_R_STATUS  0x04u
#define HS_R_HW      0x08u
#define HS_R_BM      0x10u
#define HS_R_TGIF    0x20u
#define HS_R_DMRNET  0x40u      /* the (large) DMR Network Manager page */
#define HS_R_ALL     0x7Fu
#define HS_C_CONN    0x100u     /* connection state or message changed */
#define HS_C_ACTION  0x200u     /* an action finished */
#define HS_C_SCAN    0x400u     /* the search for a hotspot made progress */

typedef enum {
    HS_ACT_MODE = 0,        /* s1 = mode, i3 = pause(1)/resume(0) */
    HS_ACT_SYS,             /* s1 = system_api action name */
    HS_ACT_BM_LINK,         /* i1 = tg, i2 = slot, i3 = link(1)/drop(0) */
    HS_ACT_BM_DROP_DYN,     /* i2 = slot */
    HS_ACT_BM_DROP_QSO,     /* i2 = slot */
    HS_ACT_BM_ADD,          /* s1 = tg list, i2 = slot */
    HS_ACT_BM_DELETE,       /* i1 = tg, i2 = slot */
    HS_ACT_YSF,             /* s1 = host, i3 = link/unlink */
    HS_ACT_P25,             /* s1 = tg,   i3 = link/unlink */
    HS_ACT_NXDN,            /* s1 = tg,   i3 = link/unlink */
    HS_ACT_DSTAR,           /* s1 = module, s2 = ref, s3 = letter, i3 */
    HS_ACT_TGIF,            /* i1 = tg, i2 = slot, i3 = link/unlink */
    HS_ACT_DMRNET           /* s1 = net id ("net4"), i3 = enable(1)/disable(0) */
} hs_act_kind;

typedef struct {
    hs_act_kind kind;
    char        s1[40];
    char        s2[16];
    char        s3[4];
    int         i1;
    int         i2;
    int         i3;
    char        label[48];  /* used in the result message, e.g. "Pause DMR" */
} hs_action;

typedef struct {
    int            conn;            /* 1 reachable, -1 not, 0 not yet known */
    char           conn_msg[160];   /* why not, when conn < 0 */
    int            auth_failed;     /* the hotspot refused our login */
    unsigned long  updated_cs;      /* when anything last arrived */

    int            radio_ok;
    wpsd_radio     radio;
    int            hw_ok;
    wpsd_hw        hw;
    int            status_ok;
    wpsd_status    status;
    char           status_raw[3072];    /* text of an unrecognised status page */
    int            heard_ok;
    wpsd_lastheard heard;

    int            bm_state;        /* 0 unknown, 1 lists ok, 2 notice, -1 n/a */
    wpsd_bm        bm;
    int            tgif_state;      /* 0 unknown, 1 ok, -1 TGIF not in use */
    wpsd_tgif      tgif;
    int            dmrnet_state;    /* 0 unknown, 1 ok, -1 none, -2 login */
    wpsd_dmrnets   dmrnets;

    /* The search of the local network for a hotspot. */
    int            scan_state;      /* 0 idle, 1 running, 2 finished */
    int            scan_done;       /* hosts tried so far */
    int            scan_total;
    int            scan_nfound;
    char           scan_prefix[24];
    char           scan_addr[4][24];
    char           scan_info[4][64];

    char           req_line[8][112];/* how each fetch went (see hs_request_summary) */
    int            nreq;

    int            act_seq;         /* bumped when an action completes */
    int            act_ok;
    char           act_msg[224];
} hs_model;

typedef struct hs_client hs_client;

hs_client *hs_new(const hs_config *cfg);
void hs_free(hs_client *hs);

/* Replace the settings (takes effect on the next request). */
void hs_set_config(hs_client *hs, const hs_config *cfg);
const hs_config *hs_get_config(const hs_client *hs);

/* Automatic polling on/off. Turning it on schedules an immediate cycle. */
void hs_set_polling(hs_client *hs, int on, unsigned long now_cs);

/* Queue a fetch of the parts named in `mask` (HS_R_*) right away. */
void hs_refresh(hs_client *hs, unsigned mask);

/* Which extra parts (HS_R_HEARD, HS_R_HW, HS_R_BM, HS_R_TGIF, HS_R_DMRNET)
 * the polling should keep fresh - what the visible tab shows. The radio
 * state and the status page are always polled. Anything newly asked for is
 * fetched straight away. */
void hs_set_focus(hs_client *hs, unsigned parts);
unsigned hs_get_focus(const hs_client *hs);

/* Search one /24 of the local network (text such as "10.0.0.27" or
 * "10.0.0") for a hotspot. Returns 0 if started, -1 if the text is not the
 * start of an address. Progress and results appear in the model
 * (scan_*); HS_C_SCAN is reported as it goes. */
int hs_scan_start(hs_client *hs, const char *text, unsigned long now_cs);
void hs_scan_stop(hs_client *hs);
/* Call once the finished results have been dealt with. */
void hs_scan_clear(hs_client *hs);

/* Queue an action. Returns 0 if accepted, -1 if the arguments were not
 * valid or the queue is full. A refresh follows a few seconds after it. */
int hs_do(hs_client *hs, const hs_action *act);

/* Pump the state machine. Returns a mask of HS_R_* / HS_C_* bits for what
 * changed (0 if nothing). Cheap when idle. */
unsigned hs_step(hs_client *hs, unsigned long now_cs);

/* Nonzero while a request is running or queued. */
int hs_busy(const hs_client *hs);

const hs_model *hs_get(const hs_client *hs);

/* One short line per kind of request describing how the last attempt went
 * and what was made of the reply, e.g. "Status: HTTP 200, 5.6 KB - 30 items
 * in 9 sections". For showing on screen so a problem with one page can be
 * read off without a report file. Returns the number of lines (at most
 * `max`). */
int hs_request_summary(const hs_client *hs, char lines[][112], int max);

/* Human-readable report of the settings (password omitted), the model and
 * the last raw reply to each request, for pasting into a bug report. */
void hs_write_diagnostics(const hs_client *hs, FILE *f);

#endif /* HS_CLIENT_H */
