/*
 * wpsd.h
 *
 * Everything that knows how the WPSD dashboard (the W0CHP-PiStar-Dash PHP
 * code, https://repo.w0chp.net/WPSD-Dev/WPSD-WebCode) talks: the URLs and
 * form fields to send, and the parsers that turn its replies into plain C
 * structures.
 *
 * Where each piece comes from (all checked against the dashboard source,
 * not guessed):
 *
 *   GET  /api/?limit=N&names=false&country=false     JSON last heard (public)
 *   GET  /mmdvmhost/radioinfo.php                    radio state table (public)
 *   GET  /mmdvmhost/repeaterinfo.php                 mode/network "pills" (public)
 *   GET  /includes/hw_info.php                       CPU/RAM/temp cards (public)
 *   GET  /admin/bm-manager.php                       BrandMeister TG lists (auth)
 *   GET  /admin/system_api.php?action=...            WPSD's own remote API (auth)
 *   POST /admin/index.php?func=mode_man              Instant Mode Manager (auth)
 *   POST /admin/index.php?func=ysf_man|p25_man|nxdn_man|ds_man|tgif_man
 *
 * The status fragments are HTML meant for a browser; the parsers are
 * written to be tolerant (unknown sections and extra pills are kept and
 * shown rather than rejected) so a dashboard update that adds a row does
 * not break the client.
 */

#ifndef HS_WPSD_H
#define HS_WPSD_H

#include <stddef.h>

#define WPSD_MAX_SECTIONS   24
#define WPSD_MAX_PILLS      96
#define WPSD_MAX_KV         20
#define WPSD_MAX_HEARD      50
#define WPSD_MAX_BM_STATIC  40
#define WPSD_MAX_BM_DYN     16
#define WPSD_MAX_BM_SLOTS   4
#define WPSD_MAX_DMRNETS    8
#define WPSD_MAX_YSF        3000

/* ------------------------------------------------------------------ */
/* Parsed data                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PILL_ACTIVE = 0,
    PILL_PAUSED,
    PILL_ERROR,
    PILL_INACTIVE,
    PILL_OTHER
} pill_state;

typedef struct {
    char          label[40];
    char          value[80];
    unsigned char state;        /* pill_state */
    unsigned char section;      /* index into wpsd_status.sec */
} wpsd_pill;

typedef struct {
    char title[40];
} wpsd_section;

/* From repeaterinfo.php: titled groups of status pills. */
typedef struct {
    wpsd_section sec[WPSD_MAX_SECTIONS];
    int          nsec;
    wpsd_pill    pill[WPSD_MAX_PILLS];
    int          npill;
} wpsd_status;

typedef struct {
    char key[32];
    char val[96];
} wpsd_kv;

/* From radioinfo.php: a header row and a data row of cells. `state` is the
 * first data cell (IDLE / RX: DMR / TX: DMR / Standby: DMR / OFFLINE). */
typedef struct {
    wpsd_kv kv[WPSD_MAX_KV];
    int     n;
    char    state[40];
} wpsd_radio;

/* From hw_info.php: the "stat cards" (CPU load, temperature, RAM...) plus
 * the Hardware/Platform/OS/Uptime lines from the first card's tooltip. */
typedef struct {
    wpsd_kv kv[WPSD_MAX_KV];
    int     n;
} wpsd_hw;

typedef struct {
    char time[24];      /* HH:MM:SS (UTC) */
    char mode[28];
    char call[24];
    char target[40];
    char src[8];        /* "RF" or "Net" */
    char dur[12];       /* empty while the transmission is still going */
    char loss[12];
    char name[36];
    char country[28];
    int  active;
} wpsd_heard;

typedef struct {
    wpsd_heard row[WPSD_MAX_HEARD];
    int        n;
} wpsd_lastheard;

typedef struct {
    int  tg;
    int  slot;          /* value for the API (0 on a simplex hotspot) */
    int  disp_slot;     /* 1 or 2, as the dashboard shows it */
    int  linked;
    char name[48];
} wpsd_bm_static;

typedef struct {
    int  tg;
    int  disp_slot;
    char name[48];
    char timeout[40];
} wpsd_bm_dyn;

/* From bm-manager.php. `ok` means the lists are usable; otherwise `notice`
 * carries the dashboard's own explanation (no API key, BM disabled...). */
typedef struct {
    int            ok;
    char           id[16];
    char           network[48];
    wpsd_bm_static st[WPSD_MAX_BM_STATIC];
    int            nst;
    wpsd_bm_dyn    dyn[WPSD_MAX_BM_DYN];
    int            ndyn;
    int            drop_slot[WPSD_MAX_BM_SLOTS];   /* slots with Drop buttons */
    int            ndrop;
    char           notice[160];
} wpsd_bm;

/* From tgif_links.php: the talkgroup the hotspot is linked to on each DMR
 * timeslot at TGIF (the page is empty when TGIF is not in use). */
typedef struct {
    int  ok;
    char id[16];
    int  tg[2];         /* slot 1, slot 2; 0 = none */
    char name[2][40];
} wpsd_tgif;

/* From the DMR Network Manager (DMRGateway hotspots: BrandMeister, TGIF,
 * XLX... running side by side). `id` is what system_api.php wants as
 * dmrNet ("net1", "net4", "xlx"). */
typedef struct {
    char id[12];
    char name[48];
    int  enabled;
} wpsd_dmrnet;

typedef struct {
    wpsd_dmrnet net[WPSD_MAX_DMRNETS];
    int         n;
} wpsd_dmrnets;

/* From the YSF Link Manager page: the reflectors and FCS rooms the hotspot
 * can link to (its YSFHosts.txt / FCSHosts.txt), in the order it lists them. */
typedef struct {
    char value[12];     /* what the form sends: "YSF00001", "FCS00123" */
    char text[84];      /* "Parrot", "UK-Calling - United Kingdom" */
} wpsd_ysf;

typedef struct {
    wpsd_ysf *e;        /* malloc'd; free with wpsd_ysflist_free */
    int       n;
    int       cap;
} wpsd_ysflist;

/* The outcome of an action, boiled down for display. */
typedef struct {
    int  ok;
    char msg[160];
} wpsd_result;

/* ------------------------------------------------------------------ */
/* Parsers. All return 0 if the text is not recognisable.             */
/* ------------------------------------------------------------------ */

int wpsd_parse_status(const char *html, size_t n, wpsd_status *out);
int wpsd_parse_radio(const char *html, size_t n, wpsd_radio *out);
int wpsd_parse_hw(const char *html, size_t n, wpsd_hw *out);
int wpsd_parse_heard(const char *json, size_t n, wpsd_lastheard *out);
int wpsd_parse_bm(const char *html, size_t n, wpsd_bm *out);
int wpsd_parse_tgif(const char *html, size_t n, wpsd_tgif *out);
int wpsd_parse_dmrnets(const char *html, size_t n, wpsd_dmrnets *out);

/* Replaces what `out` holds (which must be zero-initialised the first time).
 * Returns the number of reflectors found, 0 if the page has no list. */
int wpsd_parse_ysflist(const char *html, size_t n, wpsd_ysflist *out);
void wpsd_ysflist_free(wpsd_ysflist *l);

/* system_api.php replies ({"output":[..],"exit_status":0},
 * {"success":true}, {"error":".."}, {"ip":".."}). */
int wpsd_parse_api_result(const char *json, size_t n, wpsd_result *out);

/* The page returned after a manager form POST: looks for the manager's
 * "<prefix>-alert" box (prefix is imm, ysf, p25, nxdn, dstar or tgif). */
int wpsd_parse_manager_reply(const char *html, size_t n, const char *prefix,
                             wpsd_result *out);

/* ------------------------------------------------------------------ */
/* Request construction                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *method;         /* "GET" or "POST" */
    const char *ctype;          /* NULL for GET */
    char        target[256];
    char        body[400];
    int         auth;           /* needs the admin login */
    const char *reply_prefix;   /* for wpsd_parse_manager_reply, or NULL */
    int         json_reply;     /* parse as system_api JSON */
} wpsd_request;

/* Each returns 0 on success, -1 if the arguments do not fit/are invalid. */
int wpsd_req_heard(wpsd_request *rq, int limit, int names);
int wpsd_req_radio(wpsd_request *rq);
int wpsd_req_status(wpsd_request *rq);
int wpsd_req_hw(wpsd_request *rq);
int wpsd_req_bm_page(wpsd_request *rq);
int wpsd_req_tgif_links(wpsd_request *rq);
/* The YSF Link Manager page, which holds the list of reflectors. */
int wpsd_req_ysflist(wpsd_request *rq);
/* The DMR Network Manager only exists inside the admin page. */
int wpsd_req_dmrnets(wpsd_request *rq);
/* netid: "net1".."net9" or "xlx", as listed by wpsd_parse_dmrnets. */
int wpsd_req_dmrnet_set(wpsd_request *rq, const char *netid, int enable);

/* mode: "DMR", "YSF", "D-Star", "P25", "NXDN" (the pill labels). */
int wpsd_req_mode(wpsd_request *rq, const char *mode, int pause);

/* action: restart_wpsd_services, reboot, shutdown, update_hostfiles,
 * stop_wpsd_services, get_ip ... (see system_api.php). */
int wpsd_req_sysapi(wpsd_request *rq, const char *action);

int wpsd_req_bm_link(wpsd_request *rq, int tg, int slot, int link);
int wpsd_req_bm_drop_dynamic(wpsd_request *rq, int slot);
int wpsd_req_bm_drop_qso(wpsd_request *rq, int slot);
int wpsd_req_bm_add(wpsd_request *rq, const char *tgs, int slot);
int wpsd_req_bm_delete(wpsd_request *rq, int tg, int slot);

/* host: "YSF00001", "FCS00123" (or "none" to unlink). */
int wpsd_req_ysf(wpsd_request *rq, const char *host, int link);
/* tg: a talkgroup number, or "none" to unlink. */
int wpsd_req_p25(wpsd_request *rq, const char *tg, int link);
int wpsd_req_nxdn(wpsd_request *rq, const char *tg, int link);
/* module: the gateway's 8 character "CALL   B" radio module string (as
 * shown by the RPT1 pill); ref: 6 character reflector name; letter: its
 * module letter. */
int wpsd_req_dstar(wpsd_request *rq, const char *module, const char *ref,
                   const char *letter, int link);
int wpsd_req_tgif(wpsd_request *rq, int tg, int slot, int link);

#endif /* HS_WPSD_H */
