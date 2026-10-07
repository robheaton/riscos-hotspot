/*
 * it_client.c
 *
 * Integration tests: the real client and HTTP code over real TCP sockets
 * against mock_wpsd.py (and, for --split, split_server.py).
 *
 *   it_client PORT            full client scenarios against the mock hotspot
 *   it_client --split PORT    HTTP framing with the reply cut at every byte
 *
 * Linked against a build of client.c with short timeouts, so the timeout
 * cases take a fraction of a second.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "http.h"
#include "rows.h"
#include "util.h"

static int failures;
static int checks;

static void check_at(const char *file, int line, int ok, const char *expr)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(c) check_at(__FILE__, __LINE__, (c), #c)

static int g_port;
static int g_second;       /* a second mock hotspot is listening on 127.0.0.77 */

static hs_config base_config(void)
{
    hs_config cfg;

    hs_config_defaults(&cfg);
    u_copy(cfg.host, sizeof cfg.host, "127.0.0.1");
    cfg.port = g_port;
    u_copy(cfg.user, sizeof cfg.user, "pi-star");
    u_copy(cfg.pass, sizeof cfg.pass, "raspberry");
    cfg.heard_rows = 15;
    return cfg;
}

/* Runs the client until `done` is true or `secs` pass. */
static int pump(hs_client *hs, int (*done)(const hs_model *), double secs)
{
    unsigned long start = http_clock_cs();

    while ((http_clock_cs() - start) < (unsigned long)(secs * 100)) {
        hs_step(hs, http_clock_cs());
        if (done != NULL && done(hs_get(hs)))
            return 1;
        if (done == NULL && !hs_busy(hs))
            return 1;
        usleep(2000);
    }

    return 0;
}

/* One blocking GET via the same HTTP code (for the mock's control URLs). */
static char *simple_get(const char *target)
{
    http_endpoint ep;
    http_req *r;
    char *out;
    size_t len;
    const char *body;
    unsigned long start = http_clock_cs();

    memset(&ep, 0, sizeof ep);
    u_copy(ep.host, sizeof ep.host, "127.0.0.1");
    ep.port = g_port;

    r = http_start(&ep, "GET", target, NULL, NULL, 0, 1u << 20, 500);
    while (http_step(r) != HTTP_DONE && http_step(r) != HTTP_FAILED) {
        if (http_clock_cs() - start > 500)
            break;
        usleep(1000);
    }

    body = http_body(r, &len);
    out = (char *)malloc(len + 1);
    memcpy(out, body, len);
    out[len] = '\0';
    http_free(r);
    return out;
}

static void mock(const char *setting)
{
    char target[200];
    char *r;

    snprintf(target, sizeof target, "/__mock/set?%s", setting);
    r = simple_get(target);
    free(r);
}

static int mock_log_contains(const char *needle)
{
    char *log = simple_get("/__mock/log");
    int found = strstr(log, needle) != NULL;

    free(log);
    return found;
}

/* How many times `needle` appears in the mock hotspot's log. */
static int mock_log_count(const char *needle)
{
    char *log = simple_get("/__mock/log");
    const char *p = log;
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    free(log);
    return n;
}

/* ------------------------------------------------------------------ */

static int all_but_status(const hs_model *m)
{
    return m->radio_ok && m->heard_ok && m->hw_ok && m->bm_state == 1 &&
           m->status_raw[0] != '\0';
}

static int all_present(const hs_model *m)
{
    return m->radio_ok && m->heard_ok && m->status_ok && m->hw_ok &&
           m->bm_state == 1;
}

static int act_done_1(const hs_model *m) { return m->act_seq >= 1; }
static int act_done_2(const hs_model *m) { return m->act_seq >= 2; }

static int dmr_paused(const hs_model *m)
{
    return m->status_ok && ui_mode_state(m, "DMR") == PILL_PAUSED;
}

static int dmr_active(const hs_model *m)
{
    return m->status_ok && ui_mode_state(m, "DMR") == PILL_ACTIVE;
}

static int bm_has_tg_linked(const hs_model *m, int tg, int linked)
{
    int i;

    for (i = 0; i < m->bm.nst; i++) {
        if (m->bm.st[i].tg == tg)
            return m->bm.st[i].linked == linked;
    }

    return 0;
}

static int bm_9990_linked(const hs_model *m)
{
    return m->bm_state == 1 && bm_has_tg_linked(m, 9990, 1);
}

static int bm_9990_dropped(const hs_model *m)
{
    return m->bm_state == 1 && bm_has_tg_linked(m, 9990, 0);
}

static int bm_has_5555(const hs_model *m)
{
    int i;

    for (i = 0; i < m->bm.nst; i++) {
        if (m->bm.st[i].tg == 5555)
            return 1;
    }

    return 0;
}

static int bm_no_5555(const hs_model *m)
{
    return m->bm_state == 1 && !bm_has_5555(m);
}

static int bm_no_dynamic(const hs_model *m)
{
    return m->bm_state == 1 && m->bm.ndyn == 0;
}

static int ysf_linked_test(const hs_model *m)
{
    const char *v = m->status_ok ? ui_pill_value(m, "Link") : NULL;
    int i;

    /* Both D-Star and YSF have a "Link" pill; look for the YSF one. */
    for (i = 0; i < m->status.npill; i++) {
        if (strcmp(m->status.pill[i].label, "Link") == 0 &&
            strstr(m->status.pill[i].value, "YSF00001") != NULL)
            return 1;
    }

    (void)v;
    return 0;
}

static int dstar_linked_xlx(const hs_model *m)
{
    int i;

    for (i = 0; i < m->status.npill; i++) {
        if (strcmp(m->status.pill[i].label, "Link") == 0 &&
            strstr(m->status.pill[i].value, "XLX123 D") != NULL)
            return 1;
    }

    return 0;
}

static int conn_failed(const hs_model *m)
{
    return m->conn < 0;
}

static void scenario_basic(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);

    printf("-- basic polling\n");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_present, 10.0));
    CHECK(m->conn == 1);
    CHECK(!m->auth_failed);
    CHECK(strcmp(m->radio.state, "IDLE") == 0);
    CHECK(m->status.npill == 30);
    CHECK(m->heard.n == 15);
    CHECK(m->hw.n == 10);
    CHECK(m->bm.nst == 3 && m->bm.ndyn == 1);
    CHECK(strcmp(m->bm.id, "2345678") == 0);
    CHECK(mock_log_contains("limit=15&names=false&country=false"));

    {
        char lines[8][112];
        int n = hs_request_summary(hs, lines, 8);

        CHECK(n == 6);      /* the big DMR network page is not read yet */
        CHECK(strstr(lines[0], "Radio: HTTP 200") != NULL &&
              strstr(lines[0], "understood") != NULL);
        CHECK(strstr(lines[1], "Last heard: HTTP 200") != NULL &&
              strstr(lines[1], "15 entries") != NULL);
        CHECK(strstr(lines[2], "30 items in 9 sections") != NULL);
        CHECK(strstr(lines[3], "System: HTTP 200") != NULL &&
              strstr(lines[3], "10 items") != NULL);
        CHECK(strstr(lines[4], "3 static, 1 dynamic") != NULL);
        CHECK(strstr(lines[5], "TGIF") != NULL);
    }

    /* RX shows up in the next radio poll. */
    mock("radio=RX%3A%20DMR");
    hs_refresh(hs, HS_R_RADIO | HS_R_HEARD);
    CHECK(pump(hs, NULL, 5.0));
    CHECK(strcmp(m->radio.state, "RX: DMR") == 0);
    CHECK(m->heard.row[0].active);
    mock("radio=IDLE");

    hs_free(hs);
}

static void scenario_framing(void)
{
    static const char *const variants[] = {
        "chunked=1", "no_length=1", "trickle=3", "early_eof=1", NULL
    };
    static const char *const resets[] = {
        "chunked=0", "no_length=0", "trickle=0", "early_eof=0", NULL
    };
    int v;

    for (v = 0; variants[v] != NULL; v++) {
        hs_config cfg = base_config();
        hs_client *hs = hs_new(&cfg);
        const hs_model *m = hs_get(hs);

        printf("-- framing: %s\n", variants[v]);
        mock(variants[v]);
        hs_set_polling(hs, 1, http_clock_cs());
        CHECK(pump(hs, all_present, 20.0));
        CHECK(m->status.npill == 30);
        CHECK(m->heard.n == 15);
        CHECK(m->bm.nst == 3);
        CHECK(m->conn == 1);
        hs_free(hs);
        mock(resets[v]);
    }
}

static void scenario_actions(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);
    hs_action a;

    printf("-- actions\n");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_present, 10.0));
    hs_set_polling(hs, 0, 0);

    /* Pause DMR through the Instant Mode Manager form. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_MODE;
    u_copy(a.s1, sizeof a.s1, "DMR");
    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Pause DMR");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, act_done_1, 10.0));
    CHECK(m->act_ok);
    CHECK(strstr(m->act_msg, "Paused: DMR") != NULL);
    CHECK(strstr(m->act_msg, "Pause DMR") != NULL);
    CHECK(pump(hs, dmr_paused, 10.0));      /* the follow-up refresh */

    /* Again: the dashboard says it is already paused. */
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, act_done_2, 10.0));
    CHECK(!m->act_ok);
    CHECK(strstr(m->act_msg, "already paused") != NULL);

    /* Resume. */
    a.i3 = 0;
    u_copy(a.label, sizeof a.label, "Resume DMR");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, dmr_active, 10.0));
    CHECK(m->act_ok);

    CHECK(mock_log_contains("mode_action=Pause&mode_sel=DMR&func=mode_man"));

    /* YSF link through the form, then verify it shows in the status. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_YSF;
    u_copy(a.s1, sizeof a.s1, "00001");
    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Link YSF");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, ysf_linked_test, 10.0));
    CHECK(m->act_ok);
    CHECK(strstr(m->act_msg, "Linked to YSF00001") != NULL);

    /* D-Star */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_DSTAR;
    u_copy(a.s1, sizeof a.s1, "M1ABC B");
    u_copy(a.s2, sizeof a.s2, "XLX123");
    u_copy(a.s3, sizeof a.s3, "D");
    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Link D-Star");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, dstar_linked_xlx, 10.0));

    /* BrandMeister: link / drop a static TG through system_api.php. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_BM_LINK;
    a.i1 = 9990;
    a.i2 = 0;
    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Link TG 9990");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, bm_9990_linked, 10.0));
    a.i3 = 0;
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, bm_9990_dropped, 10.0));

    /* Add (POST, answered with a redirect) and delete (GET + redirect). */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_BM_ADD;
    u_copy(a.s1, sizeof a.s1, "5555");
    u_copy(a.label, sizeof a.label, "Add TG 5555");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, bm_has_5555, 10.0));
    CHECK(m->act_ok);

    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_BM_DELETE;
    a.i1 = 5555;
    u_copy(a.label, sizeof a.label, "Delete TG 5555");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, bm_no_5555, 10.0));

    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_BM_DROP_DYN;
    u_copy(a.label, sizeof a.label, "Drop dynamic");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, bm_no_dynamic, 10.0));

    /* system_api.php: restart services returns its last output line. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_SYS;
    u_copy(a.s1, sizeof a.s1, "restart_wpsd_services");
    u_copy(a.label, sizeof a.label, "Restart services");
    {
        int seq = m->act_seq;

        CHECK(hs_do(hs, &a) == 0);
        CHECK(pump(hs, NULL, 10.0));
        CHECK(m->act_seq == seq + 1);
        CHECK(m->act_ok);
        CHECK(strstr(m->act_msg, "Starting WPSD services...done") != NULL);
    }

    /* Reboot: the hotspot drops the connection without replying. */
    mock("reboot_drop=1");
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_SYS;
    u_copy(a.s1, sizeof a.s1, "reboot");
    u_copy(a.label, sizeof a.label, "Reboot");
    {
        int seq = m->act_seq;

        CHECK(hs_do(hs, &a) == 0);
        CHECK(pump(hs, NULL, 10.0));
        CHECK(m->act_seq == seq + 1);
        CHECK(m->act_ok);
        CHECK(strstr(m->act_msg, "going offline") != NULL);
    }
    mock("reboot_drop=0");

    /* Invalid arguments are rejected before anything is sent. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_MODE;
    u_copy(a.s1, sizeof a.s1, "DMR;reboot");
    CHECK(hs_do(hs, &a) < 0);

    hs_free(hs);
}

static int bm_login_needed(const hs_model *m)
{
    return m->bm_state == -2;
}

static void scenario_login(void)
{
    hs_config cfg = base_config();
    hs_client *hs;
    const hs_model *m;
    hs_action a;

    printf("-- wrong password\n");
    u_copy(cfg.pass, sizeof cfg.pass, "wrong");
    hs = hs_new(&cfg);
    m = hs_get(hs);

    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, bm_login_needed, 10.0));
    CHECK(m->auth_failed);
    CHECK(m->conn == 1);                  /* the hotspot did answer */
    CHECK(m->radio_ok);                   /* the public pages still work */
    hs_set_polling(hs, 0, 0);

    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_SYS;
    u_copy(a.s1, sizeof a.s1, "restart_wpsd_services");
    u_copy(a.label, sizeof a.label, "Restart");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, act_done_1, 10.0));
    CHECK(!m->act_ok);
    CHECK(strstr(m->act_msg, "refused the login") != NULL);

    /* Fixing the password in Choices clears the problem. */
    u_copy(cfg.pass, sizeof cfg.pass, "raspberry");
    hs_set_config(hs, &cfg);
    CHECK(!m->auth_failed);
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_present, 10.0));
    CHECK(!m->auth_failed);

    hs_free(hs);
}

static void scenario_failures(void)
{
    hs_config cfg = base_config();
    hs_client *hs;
    const hs_model *m;

    printf("-- nothing listening\n");
    cfg.port = 1;
    hs = hs_new(&cfg);
    m = hs_get(hs);
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, conn_failed, 10.0));
    CHECK(strstr(m->conn_msg, "onnect") != NULL);
    hs_free(hs);

    printf("-- no address\n");
    cfg = base_config();
    cfg.host[0] = '\0';
    hs = hs_new(&cfg);
    m = hs_get(hs);
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, conn_failed, 5.0));
    CHECK(strstr(m->conn_msg, "no hotspot address") != NULL);
    hs_free(hs);

    printf("-- slow hotspot times out\n");
    cfg = base_config();
    hs = hs_new(&cfg);
    m = hs_get(hs);
    mock("delay_ms=1500");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, conn_failed, 10.0));
    CHECK(strstr(m->conn_msg, "timed out") != NULL);
    mock("delay_ms=0");
    hs_free(hs);

    printf("-- HTTP errors are not 'not connected'\n");
    cfg = base_config();
    hs = hs_new(&cfg);
    m = hs_get(hs);
    mock("force_status=500");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, NULL, 10.0));
    CHECK(m->conn == 1);
    CHECK(!m->radio_ok && !m->status_ok && !m->heard_ok);
    mock("force_status=0");
    hs_free(hs);
}

/* Steps the client for a fixed time, busy or not. */
static void pump_for(hs_client *hs, double secs)
{
    unsigned long start = http_clock_cs();

    while ((http_clock_cs() - start) < (unsigned long)(secs * 100)) {
        hs_step(hs, http_clock_cs());
        usleep(2000);
    }
}

static int pill_state_of(const hs_model *m, const char *label)
{
    int i;

    for (i = 0; i < m->status.npill; i++) {
        if (strcmp(m->status.pill[i].label, label) == 0)
            return m->status.pill[i].state;
    }

    return -1;
}

static int tgif_and_nets_ready(const hs_model *m)
{
    return m->tgif_state == 1 && m->dmrnet_state == 1 && m->status_ok &&
           m->bm_state == 1;
}

static int tgif_net_off(const hs_model *m)
{
    return m->dmrnet_state == 1 && m->dmrnets.n == 2 &&
           !m->dmrnets.net[1].enabled &&
           pill_state_of(m, "TGIF Network") == PILL_PAUSED;
}

static int tgif_net_on(const hs_model *m)
{
    return m->dmrnet_state == 1 && m->dmrnets.n == 2 &&
           m->dmrnets.net[1].enabled &&
           pill_state_of(m, "TGIF Network") == PILL_ACTIVE;
}

static int tgif_slot2_is_2341(const hs_model *m)
{
    return m->tgif_state == 1 && m->tgif.tg[1] == 2341;
}

static int tgif_slot2_is_none(const hs_model *m)
{
    return m->tgif_state == 1 && m->tgif.tg[1] == 0;
}

static int tgif_slot1_is_235(const hs_model *m)
{
    return m->tgif_state == 1 && m->tgif.tg[0] == 235;
}

static void scenario_tgif_and_networks(void)
{
    hs_config cfg = base_config();
    hs_client *hs;
    const hs_model *m;
    hs_action a;

    printf("-- BrandMeister + TGIF on a DMRGateway hotspot\n");
    cfg.refresh_s = 2;
    hs = hs_new(&cfg);
    m = hs_get(hs);

    mock("gateway=1");
    mock("tgif=1");
    hs_set_focus(hs, HS_R_BM | HS_R_TGIF | HS_R_DMRNET);
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, tgif_and_nets_ready, 10.0));

    CHECK(strcmp(m->tgif.id, "2345678") == 0);
    CHECK(m->tgif.tg[0] == 0 && m->tgif.tg[1] == 91);
    CHECK(strcmp(m->tgif.name[1], "Worldwide") == 0);
    CHECK(m->dmrnets.n == 2);
    CHECK(strcmp(m->dmrnets.net[0].id, "net1") == 0 &&
          strcmp(m->dmrnets.net[0].name, "BM 2341 United Kingdom") == 0);
    CHECK(strcmp(m->dmrnets.net[1].id, "net4") == 0 &&
          strcmp(m->dmrnets.net[1].name, "TGIF Network") == 0);
    CHECK(pill_state_of(m, "BM 2341 United Kingdom") == PILL_ACTIVE);
    CHECK(pill_state_of(m, "TGIF Network") == PILL_ACTIVE);

    /* Switch the TGIF network off, and on again. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_DMRNET;
    u_copy(a.s1, sizeof a.s1, "net4");
    a.i3 = 0;
    u_copy(a.label, sizeof a.label, "Disable TGIF Network");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, tgif_net_off, 10.0));
    CHECK(m->act_ok);
    CHECK(mock_log_contains("dmrNet=net4&netState=disable"));

    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Enable TGIF Network");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, tgif_net_on, 10.0));
    CHECK(mock_log_contains("dmrNet=net4&netState=enable"));

    /* A network the gateway does not have is refused ("KO"). */
    {
        int seq = m->act_seq;

        u_copy(a.s1, sizeof a.s1, "net9");
        CHECK(hs_do(hs, &a) == 0);
        CHECK(pump(hs, NULL, 10.0));
        CHECK(m->act_seq == seq + 1);
        CHECK(!m->act_ok);
    }

    /* TGIF talkgroups: link one on slot 2, then take it off. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_TGIF;
    a.i1 = 2341;
    a.i2 = 2;
    a.i3 = 1;
    u_copy(a.label, sizeof a.label, "Link TGIF TG 2341");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, tgif_slot2_is_2341, 10.0));
    CHECK(m->act_ok);
    CHECK(strstr(m->act_msg, "Talkgroup 2341 on Slot 2") != NULL);
    CHECK(mock_log_contains("tgifAction=LINK&tgifNumber=2341&tgifSlot=2"));

    a.i1 = 235;
    a.i2 = 1;
    u_copy(a.label, sizeof a.label, "Link TGIF TG 235 TS1");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, tgif_slot1_is_235, 10.0));

    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_TGIF;
    a.i2 = 2;
    a.i3 = 0;
    u_copy(a.label, sizeof a.label, "Unlink TGIF slot 2");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, tgif_slot2_is_none, 10.0));
    CHECK(mock_log_contains("tgifAction=UNLINK&tgifNumber=0&tgifSlot=2"));

    mock("tgif=0");
    mock("gateway=0");
    hs_free(hs);
}

static void scenario_focus(void)
{
    hs_config cfg = base_config();
    hs_client *hs;
    const hs_model *m;

    printf("-- polling follows what is on screen\n");
    cfg.refresh_s = 2;
    hs = hs_new(&cfg);
    m = hs_get(hs);

    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_present, 10.0));
    CHECK(pump(hs, NULL, 10.0));
    free(simple_get("/__mock/reset"));

    /* No focus: only the radio (every cycle) and status (every third). */
    pump_for(hs, 4.5);
    CHECK(mock_log_contains("/mmdvmhost/radioinfo.php"));
    CHECK(!mock_log_contains("/api/?limit="));
    CHECK(!mock_log_contains("func=dmr_man"));
    CHECK(!mock_log_contains("bm-manager.php"));
    CHECK(!mock_log_contains("hw_info.php"));

    /* Looking at the Heard tab brings the last heard list in at once. */
    free(simple_get("/__mock/reset"));
    hs_set_focus(hs, HS_R_HEARD);
    pump_for(hs, 0.5);
    CHECK(mock_log_contains("/api/?limit="));
    CHECK(!mock_log_contains("func=dmr_man"));

    CHECK(!mock_log_contains("func=ysf_man"));

    /* And the DMR tab brings the big network page, and BrandMeister. */
    free(simple_get("/__mock/reset"));
    hs_set_focus(hs, HS_R_BM | HS_R_TGIF | HS_R_DMRNET);
    pump_for(hs, 1.0);
    CHECK(mock_log_contains("func=dmr_man"));
    CHECK(mock_log_contains("bm-manager.php"));
    CHECK(mock_log_contains("tgif_links.php"));
    CHECK(m->dmrnet_state == -1);       /* this mock hotspot has no gateway */
    CHECK(m->tgif_state == -1);         /* nor TGIF */

    hs_free(hs);
}

static int ysf_ready(const hs_model *m)
{
    return m->ysf_state == 1;
}

static int ysf_refused(const hs_model *m)
{
    return m->ysf_state == -2;
}

/* The YSF tab: the reflector list comes from the YSF Link Manager page, read
 * when the tab comes up and again on a refresh - not on every poll. */
static void scenario_ysf_list(void)
{
    hs_config cfg = base_config();
    hs_client *hs;
    const hs_model *m;
    hs_action a;
    char lines[8][112];
    int n;
    int i;
    int found = 0;

    printf("-- the YSF reflector list\n");
    cfg.refresh_s = 2;
    hs = hs_new(&cfg);
    m = hs_get(hs);
    free(simple_get("/__mock/reset"));

    hs_set_polling(hs, 1, http_clock_cs());
    hs_set_focus(hs, HS_R_YSF);
    CHECK(pump(hs, ysf_ready, 10.0));
    CHECK(m->ysf.n == 353);
    CHECK(strcmp(m->ysf.e[0].value, "YSF00001") == 0);
    CHECK(strcmp(m->ysf.e[0].text, "Parrot") == 0);

    n = hs_request_summary(hs, lines, 8);
    for (i = 0; i < n; i++) {
        if (strstr(lines[i], "YSF reflectors: HTTP 200") != NULL &&
            strstr(lines[i], "353 reflectors") != NULL)
            found = 1;
    }
    CHECK(found);

    /* The diagnostics keep the part of this big page that matters (the list),
     * not its first 24 KB of style sheet. */
    {
        FILE *f = tmpfile();
        char *text;
        long size;

        CHECK(f != NULL);
        if (f != NULL) {
            hs_write_diagnostics(hs, f);
            size = ftell(f);
            rewind(f);
            text = (char *)malloc((size_t)size + 1);
            CHECK(text != NULL && fread(text, 1, (size_t)size, f) == (size_t)size);
            if (text != NULL) {
                text[size] = '\0';
                CHECK(strstr(text, "name=\"ysfLinkHost\"") != NULL);
                CHECK(strstr(text, "the part from byte") != NULL);
                CHECK(strstr(text, "YSF00001 - Parrot") != NULL);
                CHECK(strstr(text, "raspberry") == NULL);
                free(text);
            }
            fclose(f);
        }
    }

    /* Read once; several polls later it has not been read again... */
    pump_for(hs, 5.0);
    CHECK(mock_log_count("func=ysf_man") == 1);

    /* ... until a refresh asks. */
    hs_refresh(hs, HS_R_YSF);
    CHECK(pump(hs, NULL, 10.0));
    CHECK(mock_log_count("func=ysf_man") == 2);
    CHECK(m->ysf_state == 1 && m->ysf.n == 353);

    /* Linking one of the listed reflectors. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_YSF;
    u_copy(a.s1, sizeof a.s1, m->ysf.e[5].value);       /* YSF00011 */
    u_copy(a.label, sizeof a.label, "Link YSF");
    a.i3 = 1;
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, NULL, 10.0));
    CHECK(m->act_ok);
    CHECK(mock_log_contains("ysfLinkHost=YSF00011&Link=LINK"));

    /* A refused login leaves no list behind. */
    {
        hs_config bad = base_config();

        u_copy(bad.pass, sizeof bad.pass, "not the password");
        bad.refresh_s = 2;
        hs_set_config(hs, &bad);
        CHECK(m->ysf.e == NULL && m->ysf_state == 0);   /* another login: start again */

        hs_refresh(hs, HS_R_YSF);
        CHECK(pump(hs, ysf_refused, 10.0));
        CHECK(m->ysf.n == 0 && m->ysf.e == NULL);
    }

    hs_free(hs);
}

static int scan_finished(const hs_model *m)
{
    return m->scan_state == 2;
}

static int has_addr(const hs_model *m, const char *addr)
{
    int i;

    for (i = 0; i < m->scan_nfound; i++) {
        if (strcmp(m->scan_addr[i], addr) == 0)
            return 1;
    }

    return 0;
}

/* A script's output with colour codes, new lines and tabs in it must reach
 * the screen as plain text: the desktop cuts text at the first control
 * character. */
static void scenario_control_chars(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);
    hs_action a;
    int plain = 1;
    size_t i;

    printf("-- control characters in a reply\n");
    mock("ctrl_chars=1");

    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_SYS;
    u_copy(a.s1, sizeof a.s1, "restart_wpsd_services");
    u_copy(a.label, sizeof a.label, "Restart services");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, NULL, 10.0));

    CHECK(m->act_ok);
    CHECK(strcmp(m->act_msg,
                 "Restart services: Starting WPSD services...done") == 0);
    for (i = 0; m->act_msg[i] != '\0'; i++) {
        if ((unsigned char)m->act_msg[i] < 0x20)
            plain = 0;
    }
    CHECK(plain);

    mock("ctrl_chars=0");
    hs_free(hs);
}

static void scenario_scan(int second)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);
    unsigned long t0;

    printf("-- searching the network for a hotspot\n");

    CHECK(hs_scan_start(hs, "not an address", http_clock_cs()) < 0);
    CHECK(m->scan_state == 0);

    CHECK(hs_scan_start(hs, "127.0.0.9", http_clock_cs()) == 0);
    CHECK(m->scan_state == 1);
    CHECK(m->scan_total == 254);
    CHECK(strcmp(m->scan_prefix, "127.0.0.") == 0);
    CHECK(hs_busy(hs));

    t0 = http_clock_cs();
    CHECK(pump(hs, scan_finished, 60.0));
    CHECK(m->scan_done == 254);
    CHECK(has_addr(m, "127.0.0.1"));
    if (second) {
        /* 127.0.0.78 answers one byte at a time for a minute: the search
         * gives up on it after its own deadline, not when it stops. */
        CHECK(http_clock_cs() - t0 < 1500);
        CHECK(!has_addr(m, "127.0.0.78"));
    }
    CHECK(strstr(m->scan_info[0], "MMDVM_HS_Hat") != NULL);
    if (second) {
        CHECK(m->scan_nfound == 2);
        CHECK(has_addr(m, "127.0.0.77"));
    } else {
        CHECK(m->scan_nfound >= 1);
    }

    hs_scan_clear(hs);
    CHECK(m->scan_state == 0);
    CHECK(!hs_busy(hs));

    /* Stopping part way leaves nothing running. */
    CHECK(hs_scan_start(hs, "127.0.0.", http_clock_cs()) == 0);
    hs_step(hs, http_clock_cs());
    hs_scan_stop(hs);
    CHECK(m->scan_state == 0);
    CHECK(!hs_busy(hs));

    /* Nothing there: a prefix with no hotspot finds none. */
    CHECK(hs_scan_start(hs, "127.1.2.", http_clock_cs()) == 0);
    CHECK(pump(hs, scan_finished, 60.0));
    CHECK(m->scan_nfound == 0);

    hs_free(hs);
}

static void scenario_legacy(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);

    printf("-- a dashboard with markup we do not know\n");
    mock("legacy=1");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_but_status, 10.0));
    CHECK(!m->status_ok);
    CHECK(m->conn == 1);
    /* The text of the page is kept, one line per block. */
    CHECK(strstr(m->status_raw, "Mode Status\nDMR\nEnabled\nYSF\nDisabled\n"
                                "Network Status\nDMR Master\nBM_2341_United_Kingdom")
          != NULL);
    /* Everything else still works. */
    CHECK(m->radio_ok && m->heard_ok && m->hw_ok);
    CHECK(m->nreq == 6);
    CHECK(strstr(m->req_line[2], "Status: HTTP 200") != NULL &&
          strstr(m->req_line[2], "not understood") != NULL);

    mock("legacy=0");
    hs_free(hs);
}

static void scenario_redirect(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    const hs_model *m = hs_get(hs);
    hs_action a;

    printf("-- a hotspot that redirects (to https)\n");
    mock("force_status=302");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, conn_failed, 10.0));
    CHECK(strstr(m->conn_msg, "redirected to https://hotspot.example/") != NULL);
    CHECK(strstr(m->conn_msg, "plain http") != NULL);
    hs_set_polling(hs, 0, 0);

    /* An action that is answered with a redirect did not happen. */
    memset(&a, 0, sizeof a);
    a.kind = HS_ACT_SYS;
    u_copy(a.s1, sizeof a.s1, "restart_wpsd_services");
    u_copy(a.label, sizeof a.label, "Restart");
    CHECK(hs_do(hs, &a) == 0);
    CHECK(pump(hs, act_done_1, 10.0));
    CHECK(!m->act_ok);
    CHECK(strstr(m->act_msg, "redirected the request to") != NULL);

    mock("force_status=0");
    hs_free(hs);
}

static void scenario_diagnostics(void)
{
    hs_config cfg = base_config();
    hs_client *hs = hs_new(&cfg);
    FILE *f = tmpfile();
    char *buf;
    long n;

    printf("-- diagnostics\n");
    hs_set_polling(hs, 1, http_clock_cs());
    CHECK(pump(hs, all_present, 10.0));

    hs_write_diagnostics(hs, f);
    n = ftell(f);
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);

    CHECK(strstr(buf, "request: GET /api/?limit=15") != NULL);
    CHECK(strstr(buf, "request: GET /mmdvmhost/repeaterinfo.php") != NULL);
    CHECK(strstr(buf, "sidebar-section-title") != NULL);
    CHECK(strstr(buf, "raspberry") == NULL);       /* never the password */
    CHECK(strstr(buf, "password=(set)") != NULL);

    free(buf);
    hs_free(hs);
}

/* ------------------------------------------------------------------ */
/* --split: the framing parser under every possible TCP segmentation  */
/* ------------------------------------------------------------------ */

static int fetch_split(const char *kind, int cut, char *body, size_t cap,
                       int *status)
{
    http_endpoint ep;
    http_req *r;
    char target[80];
    unsigned long start = http_clock_cs();
    http_state st;
    size_t len;
    const char *b;

    memset(&ep, 0, sizeof ep);
    u_copy(ep.host, sizeof ep.host, "127.0.0.1");
    ep.port = g_port;
    snprintf(target, sizeof target, "/split?kind=%s&cut=%d", kind, cut);

    r = http_start(&ep, "GET", target, NULL, NULL, 0, 4096, 300);
    do {
        st = http_step(r);
        if (st == HTTP_DONE || st == HTTP_FAILED)
            break;
        usleep(500);
    } while (http_clock_cs() - start < 400);

    b = http_body(r, &len);
    u_copyn(body, cap, b, len);
    *status = http_status(r);
    http_free(r);
    return st == HTTP_DONE;
}

static void split_tests(void)
{
    static const struct {
        const char *kind;
        int         cuts;
    } kinds[] = {
        { "chunked", 140 }, { "length", 100 }, { "close", 100 },
        { "interim", 150 }, { "lf_only", 100 }, { "empty", 80 }, { NULL, 0 }
    };
    int k;
    int cut;
    char body[256];
    int status;

    for (k = 0; kinds[k].kind != NULL; k++) {
        int bad = 0;

        for (cut = 1; cut < kinds[k].cuts; cut++) {
            int ok = fetch_split(kinds[k].kind, cut, body, sizeof body,
                                 &status);
            const char *want = (strcmp(kinds[k].kind, "empty") == 0)
                                   ? "" : "Hello, World";

            if (!ok || status != 200 || strcmp(body, want) != 0) {
                bad++;
                if (bad < 4)
                    printf("   %s cut=%d: ok=%d status=%d body=\"%s\"\n",
                           kinds[k].kind, cut, ok, status, body);
            }
        }

        printf("-- split %s: %d bad of %d\n", kinds[k].kind, bad,
               kinds[k].cuts - 1);
        CHECK(bad == 0);
    }
}

/* --hostile: replies a sane hotspot never sends. */

typedef struct {
    http_state state;
    char       err[160];
    int        status;
    size_t     body_len;
    int        truncated;
    int        incomplete;
} outcome;

static outcome hostile_fetch(const char *kind, unsigned timeout_cs,
                             size_t max_body)
{
    http_endpoint ep;
    http_req *r;
    char target[80];
    unsigned long start = http_clock_cs();
    outcome o;
    http_state st;

    memset(&o, 0, sizeof o);
    memset(&ep, 0, sizeof ep);
    u_copy(ep.host, sizeof ep.host, "127.0.0.1");
    ep.port = g_port;
    snprintf(target, sizeof target, "/split?kind=%s", kind);

    r = http_start(&ep, "GET", target, NULL, NULL, 0, max_body, timeout_cs);
    do {
        st = http_step(r);
        if (st == HTTP_DONE || st == HTTP_FAILED)
            break;
        usleep(500);
    } while (http_clock_cs() - start < 1000);

    o.state = st;
    u_copy(o.err, sizeof o.err, http_error(r));
    o.status = http_status(r);
    http_body(r, &o.body_len);
    o.truncated = http_truncated(r);
    o.incomplete = http_incomplete(r);
    http_free(r);
    return o;
}

static void hostile_tests(void)
{
    outcome o;

    printf("-- hostile: endless header block\n");
    o = hostile_fetch("hdrflood", 300, 4096);
    CHECK(o.state == HTTP_FAILED);
    CHECK(strstr(o.err, "header too large") != NULL);

    printf("-- hostile: absurd chunk size\n");
    o = hostile_fetch("badchunk", 300, 4096);
    CHECK(o.state == HTTP_FAILED);
    CHECK(strstr(o.err, "bad chunk size") != NULL);

    printf("-- hostile: Content-Length far larger than what is sent\n");
    o = hostile_fetch("hugelen", 300, 4096);
    CHECK(o.state == HTTP_DONE);
    CHECK(o.status == 200 && o.body_len == 12 && o.incomplete);

    printf("-- hostile: a 3 MB body with a 4 KB cap\n");
    o = hostile_fetch("bigbody", 300, 4096);
    CHECK(o.state == HTTP_DONE);
    CHECK(o.status == 200 && o.body_len == 4096 && o.truncated);

    printf("-- hostile: connection reset mid-body\n");
    o = hostile_fetch("reset", 300, 4096);
    CHECK(o.state == HTTP_DONE);
    CHECK(o.status == 200 && o.body_len == 7 && o.incomplete);

    printf("-- hostile: accepted but never answered\n");
    o = hostile_fetch("nothing", 60, 4096);
    CHECK(o.state == HTTP_FAILED);
    CHECK(strstr(o.err, "timed out") != NULL);

    printf("-- hostile: not HTTP at all\n");
    o = hostile_fetch("garbage", 300, 4096);
    CHECK(o.state == HTTP_FAILED);
    CHECK(strstr(o.err, "not an HTTP response") != NULL);
}

int main(int argc, char **argv)
{
    int split = 0;
    int hostile = 0;

    if (argc >= 3 && strcmp(argv[1], "--split") == 0) {
        split = 1;
        g_port = atoi(argv[2]);
    } else if (argc >= 3 && strcmp(argv[1], "--hostile") == 0) {
        hostile = 1;
        g_port = atoi(argv[2]);
    } else if (argc >= 2) {
        g_port = atoi(argv[1]);
        g_second = (argc >= 3 && strcmp(argv[2], "--second") == 0);
    } else {
        fprintf(stderr, "usage: %s [--split] PORT\n", argv[0]);
        return 2;
    }

    http_init();

    if (hostile) {
        hostile_tests();
    } else if (split) {
        split_tests();
    } else {
        scenario_basic();
        scenario_framing();
        scenario_actions();
        scenario_login();
        scenario_failures();
        scenario_redirect();
        scenario_legacy();
        scenario_tgif_and_networks();
        scenario_control_chars();
        scenario_ysf_list();
        scenario_focus();
        scenario_scan(g_second);
        scenario_diagnostics();
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
