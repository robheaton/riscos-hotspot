/*
 * ro_choices.c
 *
 * The Choices file and the diagnostics report. Follows the usual RISC OS
 * convention (and the way SFLib does it): read from Choices:Hotspot.Choices,
 * write to <Choices$Write>.Hotspot.Choices, falling back to the application
 * directory if there is no Choices$Write.
 *
 * The password is stored in the file in plain text, like most RISC OS
 * programs that need one; the file says so.
 */

#include "ro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oslib/osfile.h"
#include "util.h"

#define CHOICES_SUBDIR "Hotspot"

/* Builds the full pathname to write `leaf` to. */
static void write_path(char *file, size_t cap, const char *leaf)
{
    fileswitch_object_type type;
    int var_len = 0;

    os_read_var_val_size("Choices$Write", 0, os_VARTYPE_STRING, &var_len,
                         NULL);

    if (var_len == 0) {
        snprintf(file, cap, "<Hotspot$Dir>.%s", leaf);
        return;
    }

    snprintf(file, cap, "<Choices$Write>.%s", CHOICES_SUBDIR);
    if (xosfile_read_no_path(file, &type, NULL, NULL, NULL, NULL) == NULL &&
        type == fileswitch_NOT_FOUND)
        xosfile_create_dir(file, 0);

    snprintf(file, cap, "<Choices$Write>.%s.%s", CHOICES_SUBDIR, leaf);
}

static int read_path(char *file, size_t cap)
{
    fileswitch_object_type type;

    snprintf(file, cap, "Choices:%s.Choices", CHOICES_SUBDIR);
    if (xosfile_read_no_path(file, &type, NULL, NULL, NULL, NULL) == NULL &&
        type == fileswitch_IS_FILE)
        return 1;

    snprintf(file, cap, "<Hotspot$Dir>.Choices");
    if (xosfile_read_no_path(file, &type, NULL, NULL, NULL, NULL) == NULL &&
        type == fileswitch_IS_FILE)
        return 1;

    return 0;
}

/* The remembered link targets, one list per ui_proto, as "a,b,c". */
static const char *const recent_keys[HS_MRU_PROTOS] = {
    "RecentYSF", "RecentP25", "RecentNXDN", "RecentDStar", "RecentTGIF"
};

static void load_recent(hs_config *cfg, int proto, char *val)
{
    int n = 0;
    char *p = val;

    memset(cfg->recent[proto], 0, sizeof cfg->recent[proto]);

    while (p != NULL && *p != '\0' && n < HS_MRU_N) {
        char *comma = strchr(p, ',');

        if (comma != NULL)
            *comma = '\0';

        u_trim(p);
        if (*p != '\0')
            u_copy(cfg->recent[proto][n++], HS_MRU_LEN, p);

        p = (comma != NULL) ? comma + 1 : NULL;
    }
}

int choices_load(hs_config *cfg)
{
    char file[256];
    char line[256];
    int configured = 0;
    FILE *in;

    hs_config_defaults(cfg);

    if (!read_path(file, sizeof file))
        return 0;

    in = fopen(file, "r");
    if (in == NULL)
        return 0;

    while (fgets(line, sizeof line, in) != NULL) {
        char *eq;
        char *key;
        char *val;

        if (line[0] == '#')
            continue;

        eq = strchr(line, '=');
        if (eq == NULL)
            continue;

        *eq = '\0';
        key = u_trim(line);
        val = u_trim(eq + 1);

        if (u_ieq(key, "Host"))
            u_copy(cfg->host, sizeof cfg->host, val);
        else if (u_ieq(key, "Port"))
            cfg->port = atoi(val);
        else if (u_ieq(key, "User"))
            u_copy(cfg->user, sizeof cfg->user, val);
        else if (u_ieq(key, "Password"))
            u_copy(cfg->pass, sizeof cfg->pass, val);
        else if (u_ieq(key, "Refresh"))
            cfg->refresh_s = atoi(val);
        else if (u_ieq(key, "HeardRows"))
            cfg->heard_rows = atoi(val);
        else if (u_ieq(key, "ShowBM"))
            cfg->show_bm = atoi(val) != 0;
        else if (u_ieq(key, "Names"))
            cfg->names = atoi(val) != 0;
        else if (u_ieq(key, "Configured"))
            configured = atoi(val) != 0;
        else {
            int proto;

            for (proto = 0; proto < HS_MRU_PROTOS; proto++) {
                if (u_ieq(key, recent_keys[proto]))
                    load_recent(cfg, proto, val);
            }
        }
    }

    fclose(in);

    /* Only a file this program wrote says "Configured": the copy shipped in
     * the application directory holds defaults (the usual address of the
     * hotspot) but the first run should still offer them for checking. */
    return configured;
}

const char *choices_save(const hs_config *cfg)
{
    static char err[300];
    char file[256];
    FILE *out;

    write_path(file, sizeof file, "Choices");

    out = fopen(file, "w");
    if (out == NULL) {
        snprintf(err, sizeof err, "Could not write the Choices file (%s).",
                 file);
        return err;
    }

    fprintf(out, "# >Choices for %s\n", APP_NAME);
    fprintf(out, "# The password is stored here in plain text.\n\n");
    fprintf(out, "Configured=1\n");
    fprintf(out, "Host=%s\n", cfg->host);
    fprintf(out, "Port=%d\n", cfg->port);
    fprintf(out, "User=%s\n", cfg->user);
    fprintf(out, "Password=%s\n", cfg->pass);
    fprintf(out, "Refresh=%d\n", cfg->refresh_s);
    fprintf(out, "HeardRows=%d\n", cfg->heard_rows);
    fprintf(out, "ShowBM=%d\n", cfg->show_bm ? 1 : 0);
    fprintf(out, "Names=%d\n", cfg->names ? 1 : 0);

    {
        int proto;
        int i;

        for (proto = 0; proto < HS_MRU_PROTOS; proto++) {
            if (cfg->recent[proto][0][0] == '\0')
                continue;

            fprintf(out, "%s=", recent_keys[proto]);
            for (i = 0; i < HS_MRU_N && cfg->recent[proto][i][0] != '\0'; i++)
                fprintf(out, "%s%s", (i > 0) ? "," : "", cfg->recent[proto][i]);
            fprintf(out, "\n");
        }
    }

    if (fclose(out) != 0) {
        snprintf(err, sizeof err, "Could not finish writing %s.", file);
        return err;
    }

    xosfile_set_type(file, 0xFFF);      /* Text */
    return NULL;
}

const char *diag_save(const hs_client *hs, char *path_out, size_t cap)
{
    static char err[300];
    char file[256];
    FILE *out;

    write_path(file, sizeof file, "Diagnostics");

    out = fopen(file, "w");
    if (out == NULL) {
        snprintf(err, sizeof err, "Could not write the report (%s).", file);
        return err;
    }

    fprintf(out, "%s %s\n\n", APP_NAME, APP_VERSION);
    hs_write_diagnostics(hs, out);

    if (fclose(out) != 0) {
        snprintf(err, sizeof err, "Could not finish writing %s.", file);
        return err;
    }

    xosfile_set_type(file, 0xFFF);

    /* Show where it really is, not "<Choices$Write>.Hotspot.Diagnostics".
     * (GSTrans leaves its result unterminated.) */
    {
        char expanded[256];
        int used = 0;

        if (xos_gs_trans(file, expanded, (int)sizeof expanded - 1, &used,
                         NULL) == NULL && used > 0) {
            expanded[used] = '\0';
            u_copy(path_out, cap, expanded);
        } else {
            u_copy(path_out, cap, file);
        }
    }

    return NULL;
}
