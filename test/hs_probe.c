/*
 * hs_probe.c
 *
 * Checks the client against a REAL hotspot from a Linux machine: fetches the
 * public status pages (and the BrandMeister page too if a password is
 * given), then prints how each request went and what the parsers made of it.
 * Only ever reads - nothing it does changes anything on the hotspot.
 *
 *   hs_probe HOST [-p PORT] [-u USER] [-w PASSWORD] [-a] [-d]
 *
 * -a shows every tab as the window would, not just Status.
 * -d also prints the full diagnostics report (every reply, password omitted).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "http.h"
#include "rows.h"
#include "util.h"

/* What the window would show for `view`, as text. */
static void print_view(const hs_model *m, const hs_config *cfg, ui_view view)
{
    ui_rows rows;
    int i;
    int b;

    ui_rows_init(&rows);
    ui_rows_build(&rows, m, cfg, view);
    printf("\n---- the %s tab (%d rows) ----\n", ui_view_name(view), rows.n);

    for (i = 0; i < rows.n; i++) {
        const ui_row *r = &rows.row[i];

        switch (r->kind) {
            case UR_HEAD:
                printf("== %s", r->col[0]);
                break;
            case UR_KV:
                printf("   %-28s %s", r->col[0], r->col[1]);
                break;
            case UR_NOTE:
                printf("   %s", r->col[0]);
                break;
            case UR_TABS:
                printf("[tabs]");
                break;
            case UR_HEARD:
                printf("   %-9s %-26s %-10s %-16s %s", r->col[0], r->col[1],
                       r->col[2], r->col[3], r->col[4]);
                break;
            case UR_BM:
                printf("   %-8s %-4s %-30s %s", r->col[0], r->col[1],
                       r->col[2], r->col[3]);
                break;
            default:
                continue;
        }

        for (b = 0; b < r->nbtn; b++)
            printf("  [%s]", r->btn[b].label);
        printf("\n");
    }

    ui_rows_free(&rows);
}

int main(int argc, char **argv)
{
    hs_config cfg;
    hs_client *hs;
    const hs_model *m;
    int dump = 0;
    int all = 0;
    int bm = 0;
    int i;
    unsigned long start;
    char lines[8][112];
    int n;

    if (argc < 2) {
        fprintf(stderr, "usage: %s HOST [-p PORT] [-u USER] [-w PASSWORD] [-d]\n",
                argv[0]);
        return 2;
    }

    hs_config_defaults(&cfg);
    u_copy(cfg.host, sizeof cfg.host, argv[1]);

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            cfg.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
            u_copy(cfg.user, sizeof cfg.user, argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            u_copy(cfg.pass, sizeof cfg.pass, argv[++i]);
            bm = 1;
        } else if (strcmp(argv[i], "-d") == 0)
            dump = 1;
        else if (strcmp(argv[i], "-a") == 0)
            all = 1;
    }

    http_init();
    hs = hs_new(&cfg);
    m = hs_get(hs);

    hs_refresh(hs, HS_R_RADIO | HS_R_HEARD | HS_R_STATUS | HS_R_HW |
                   HS_R_TGIF | (bm ? HS_R_BM : 0));

    start = http_clock_cs();
    while ((http_clock_cs() - start) < 2500) {
        hs_step(hs, http_clock_cs());
        if (!hs_busy(hs))
            break;
        usleep(2000);
    }

    printf("host %s:%d  connection: %s%s%s\n\n", cfg.host, cfg.port,
           m->conn > 0 ? "ok" : (m->conn < 0 ? "FAILED - " : "unknown"),
           m->conn < 0 ? m->conn_msg : "", m->auth_failed ? " (login refused)" : "");

    n = hs_request_summary(hs, lines, 8);
    for (i = 0; i < n; i++)
        printf("  %s\n", lines[i]);

    if (m->radio_ok)
        printf("\nradio state: %s\n", m->radio.state);

    if (all) {
        print_view(m, &cfg, VIEW_STATUS);
        print_view(m, &cfg, VIEW_HEARD);
        print_view(m, &cfg, VIEW_DMR);
        print_view(m, &cfg, VIEW_LINKS);
        print_view(m, &cfg, VIEW_SYSTEM);
    } else {
        print_view(m, &cfg, VIEW_STATUS);

        if (m->heard_ok) {
            printf("\n---- last heard (%d) ----\n", m->heard.n);
            for (i = 0; i < m->heard.n; i++)
                printf("   %-9s %-10s %-12s %-14s %s %s\n",
                       m->heard.row[i].time, m->heard.row[i].mode,
                       m->heard.row[i].call, m->heard.row[i].target,
                       m->heard.row[i].dur, m->heard.row[i].src);
        }
    }

    if (dump) {
        printf("\n==== full diagnostics ====\n");
        hs_write_diagnostics(hs, stdout);
    }

    i = (m->conn > 0) ? 0 : 1;      /* before the model is freed */
    hs_free(hs);
    return i;
}
