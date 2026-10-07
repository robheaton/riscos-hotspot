/*
 * fuzz_parsers.c
 *
 * Throws mutated copies of the sample replies at every parser (HTML, JSON and
 * the WPSD readers) under ASan/UBSan. Each input is a malloc of exactly its
 * length with no terminator behind it, so a read past the end is caught.
 * The parsers see whatever a hotspot (or something pretending to be one)
 * sends, so none of this may crash, hang or overrun.
 *
 * usage: fuzz_parsers FIXTURE_DIR [ITERATIONS [SEED]]
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "html.h"
#include "json.h"
#include "rows.h"
#include "scan.h"
#include "util.h"
#include "wpsd.h"

static unsigned long long rng = 88172645463325252ULL;

static unsigned rnd(unsigned n)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (unsigned)(((rng * 2685821657736338717ULL) >> 33) % n);
}

static const char *const bits[] = {
    "<div", "</div>", "<div class='status-pill active'>", "</span>",
    "<span class='pill-value'>", "<span>", "&#", "&amp", "&#x", "\"", "'",
    "{", "[", "]", "}", "\\u", "\\ud83d", "\xE2\x80", "\xF0\x9F",
    "status-pill", "sidebar-section-title", "bm-list-row", "bm-col-tg",
    "class=\"bm-list-row\"", "divTableCell", "divTableHeadCell", "stat-label",
    "stat-value", "<!--", "-->", "<script>", "</script>", "<style>",
    "data-tg=\"", "data-slot=\"", "checked=\"checked\"", "-alert", "<br",
    "<a href=", "Dynamic Talkgroups", "ID:", "Connected To:",
    "cmd=drop_qso&slot=", "\r\n", "\0", "\xFF\xFE", NULL
};

static char *mutate(const char *src, size_t len, size_t *out_len)
{
    size_t cap = len * 2 + 256;
    char *buf = (char *)malloc(cap);
    size_t n = len;
    int ops = 1 + (int)rnd(5);
    int i;

    memcpy(buf, src, len);

    for (i = 0; i < ops; i++) {
        size_t a;
        size_t b;
        const char *frag;
        size_t fl;

        if (n == 0)
            break;

        switch (rnd(7)) {
            case 0:                         /* flip a byte */
                buf[rnd((unsigned)n)] = (char)rnd(256);
                break;
            case 1:                         /* truncate */
                n = rnd((unsigned)n);
                break;
            case 2:                         /* delete a range */
                a = rnd((unsigned)n);
                b = a + rnd((unsigned)(n - a) + 1);
                memmove(buf + a, buf + b, n - b);
                n -= b - a;
                break;
            case 3:                         /* duplicate a range */
                a = rnd((unsigned)n);
                b = a + rnd((unsigned)(n - a) + 1);
                if (n + (b - a) + 1 < cap) {
                    memmove(buf + b + (b - a), buf + b, n - b);
                    memcpy(buf + b, buf + a, b - a);
                    n += b - a;
                }
                break;
            case 4:                         /* insert an interesting fragment */
            case 5:
                frag = bits[rnd((unsigned)(sizeof bits / sizeof bits[0]) - 1)];
                fl = strlen(frag);
                if (fl == 0)
                    fl = 1;             /* the "\0" entry: one NUL byte */
                a = rnd((unsigned)n + 1);
                if (n + fl + 1 < cap) {
                    memmove(buf + a + fl, buf + a, n - a);
                    memcpy(buf + a, frag, fl);
                    n += fl;
                }
                break;
            default:                        /* overwrite with a fragment */
                frag = bits[rnd((unsigned)(sizeof bits / sizeof bits[0]) - 1)];
                fl = strlen(frag);
                if (fl == 0)
                    fl = 1;
                a = rnd((unsigned)n);
                if (a + fl > n)
                    fl = n - a;
                memcpy(buf + a, frag, fl);
                break;
        }
    }

    /* Exactly n bytes, nothing behind them. */
    {
        char *exact = (char *)malloc(n > 0 ? n : 1);

        memcpy(exact, buf, n);
        free(buf);
        *out_len = n;
        return exact;
    }
}

static wpsd_status st;
static wpsd_radio ra;
static wpsd_hw hw;
static wpsd_lastheard lh;
static wpsd_bm bm;
static wpsd_tgif tgif;
static wpsd_dmrnets nets;
static wpsd_ysflist ysf;
static hs_model model;
static ui_rows rows;

/* Whatever the parsers made of the input goes through the row builder, the
 * way the window would show it: a hostile reply must not be able to run the
 * display code off the end of anything. */
static void exercise_rows(void)
{
    hs_config cfg;
    int v;

    hs_config_defaults(&cfg);
    u_copy(cfg.host, sizeof cfg.host, "10.0.0.27");
    hs_mru_push(&cfg, PROTO_YSF, "00001");
    hs_mru_push(&cfg, PROTO_TGIF, "2341/2");
    hs_mru_push(&cfg, PROTO_DSTAR, "REF001 C");

    /* Every tab, the YSF one with and without a search. */
    for (v = 0; v < VIEW_COUNT; v++)
        ui_rows_build(&rows, &model, &cfg, (ui_view)v);
    ui_set_ysf_filter("a");
    ui_rows_build(&rows, &model, &cfg, VIEW_YSF);
    ui_set_ysf_filter("");
}

/* What a person might type into the Find hotspot box (a C string). */
static void exercise_prefix(const char *p, size_t n)
{
    char text[48];
    char out[32];
    size_t k = n < sizeof text - 1 ? n : sizeof text - 1;

    memcpy(text, p, k);
    text[k] = '\0';
    scan_prefix_from(text, out, sizeof out);
}

static void exercise(const char *p, size_t n)
{
    static const char *const prefixes[] = { "imm", "ysf", "p25", "nxdn",
                                            "dstar", "tgif" };
    wpsd_result res;
    char out[200];
    jnode *j;
    size_t i;

    wpsd_parse_status(p, n, &st);
    wpsd_parse_radio(p, n, &ra);
    wpsd_parse_hw(p, n, &hw);
    wpsd_parse_heard(p, n, &lh);
    wpsd_parse_bm(p, n, &bm);
    wpsd_parse_tgif(p, n, &tgif);
    wpsd_parse_dmrnets(p, n, &nets);
    wpsd_parse_ysflist(p, n, &ysf);
    wpsd_parse_api_result(p, n, &res);
    exercise_prefix(p, n);

    /* The same input, taken as each kind of page, shown in every tab. */
    wpsd_ysflist_free(&model.ysf);
    memset(&model, 0, sizeof model);
    model.conn = 1;
    model.status_ok = wpsd_parse_status(p, n, &model.status) > 0;
    model.radio_ok = wpsd_parse_radio(p, n, &model.radio);
    model.hw_ok = wpsd_parse_hw(p, n, &model.hw);
    model.heard_ok = wpsd_parse_heard(p, n, &model.heard);
    model.bm_state = wpsd_parse_bm(p, n, &model.bm) ? 1 : -1;
    model.tgif_state = wpsd_parse_tgif(p, n, &model.tgif) ? 1 : -1;
    model.dmrnet_state = wpsd_parse_dmrnets(p, n, &model.dmrnets) ? 1 : -1;
    model.ysf_state = wpsd_parse_ysflist(p, n, &model.ysf) > 0 ? 1 : -1;
    exercise_rows();

    for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        wpsd_parse_manager_reply(p, n, prefixes[i], &res);

    j = json_parse(p, n);
    json_free(j);

    html_text(p, n, out, sizeof out);
    if (n > 7)
        html_text(p + n / 3, n / 2, out, sizeof out);
}

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[512];
    FILE *f;
    char *buf;
    long n;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    static const char *const files[] = {
        "repeaterinfo_basic.html", "repeaterinfo_allmodes.html",
        "repeaterinfo_dmr_paused.html", "radioinfo_idle.html",
        "radioinfo_tx.html", "hwinfo.html", "api_lastheard.json",
        "api_lastheard_names.json", "bm_page.html", "bm_page_duplex.html",
        "bm_notice.html", "api_result_restart.json", "api_result_error.json",
        "ysf_reply_ok.html", "mode_reply_paused.html",
        "repeaterinfo_gateway.html", "repeaterinfo_gateway_tgif_off.html",
        "tgif_links.html", "tgif_links_none.html", "tgif_links_unused.html",
        "dmr_nets.html", "dmr_nets_tgif_off.html", "dmr_nets_single.html",
        "api_result_dmrnet_ok.json", "api_result_dmrnet_ko.json",
        "ysf_man.html", "ysf_man_card.html", "ysf_man_empty.html"
    };
    size_t nfiles = sizeof files / sizeof files[0];
    char *data[32];
    size_t len[32];
    long iterations = 20000;
    size_t i;
    long it;

    if (argc < 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR [ITERATIONS [SEED]]\n", argv[0]);
        return 2;
    }

    if (argc > 2)
        iterations = atol(argv[2]);
    if (argc > 3)
        rng ^= (unsigned long long)atoll(argv[3]) * 0x9E3779B97F4A7C15ULL;

    alarm(120);         /* a hang is a failure */

    for (i = 0; i < nfiles; i++)
        data[i] = slurp(argv[1], files[i], &len[i]);

    /* The unmutated files first. */
    for (i = 0; i < nfiles; i++) {
        char *copy = (char *)malloc(len[i]);

        memcpy(copy, data[i], len[i]);
        exercise(copy, len[i]);
        free(copy);
    }

    for (it = 0; it < iterations; it++) {
        size_t k = rnd((unsigned)nfiles);
        size_t n;
        char *m;

        /* Large reply pages are mostly filler: use a slice of them. */
        if (len[k] > 20000) {
            size_t start = rnd((unsigned)(len[k] - 20000));

            m = mutate(data[k] + start, 20000, &n);
        } else {
            m = mutate(data[k], len[k], &n);
        }

        exercise(m, n);
        free(m);
    }

    for (i = 0; i < nfiles; i++)
        free(data[i]);

    ui_rows_free(&rows);
    wpsd_ysflist_free(&ysf);
    wpsd_ysflist_free(&model.ysf);

    printf("fuzzed %ld mutated inputs: no crashes, overruns or hangs\n",
           iterations);
    return 0;
}
