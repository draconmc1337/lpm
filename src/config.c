/*
 * config.c — LPM configuration parser
 *
 * Parses /etc/lpm/lpm.conf. Format is INI-style, same as pacman.conf:
 *
 *   # comment
 *   CriticalPkg = glibc gcc bash dinit lpm
 *   IgnorePkg   = linux-headers
 *   LogDir      = /var/log/lpm
 *   FilesDir    = /var/lib/lpm/files
 *
 * Values on the same line are space-separated.
 * Lines starting with '#' are comments and are ignored.
 * Unknown keys are silently ignored for forward compatibility.
 */

#include "lpm.h"
#include "config.h"
#include <ctype.h>

/* ── internal helpers ────────────────────────────────────────────────────── */

/* trim leading and trailing whitespace in-place, return pointer to result */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/*
 * parse_list — split a space-separated value string into out[][].
 * e.g. "glibc gcc bash" -> out[0]="glibc", out[1]="gcc", out[2]="bash"
 * Returns number of tokens written, up to max.
 */
static int parse_list(const char *val, char out[][64], int max) {
    int n = 0;
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", val);
    char *tok = strtok(buf, " \t");
    while (tok && n < max) {
        snprintf(out[n++], 64, "%s", tok);
        tok = strtok(NULL, " \t");
    }
    return n;
}

/* ── defaults ────────────────────────────────────────────────────────────── */

static void set_defaults(LpmConfig *cfg) {
    cfg->n_critical = 0;
    cfg->n_ignore   = 0;
    snprintf(cfg->log_dir,   sizeof(cfg->log_dir),   "%s", LPM_LOG_DIR);
    snprintf(cfg->files_dir, sizeof(cfg->files_dir), "%s", LPM_FILES_DIR);
}

/* ── lpm_config_write_default ────────────────────────────────────────────── */

void lpm_config_write_default(void) {
    mkdir(LPM_CONF_DIR, 0755);

    FILE *f = fopen(LPM_CONF_PATH, "w");
    if (!f) {
        fprintf(stderr,
            "\033[1;33mwarning:\033[0m "
            "could not write default config to %s\n", LPM_CONF_PATH);
        return;
    }

    fprintf(f,
        "#\n"
        "# /etc/lpm/lpm.conf — Lotus Package Manager configuration\n"
        "#\n"
        "# CriticalPkg\n"
        "#   Packages that require triple-YES confirmation before removal.\n"
        "#   Equivalent to pacman's HoldPkg but with tiered UX.\n"
        "#   Space-separated list.\n"
        "#\n"
        "CriticalPkg = glibc gcc binutils bash coreutils sed grep gawk\n"
        "CriticalPkg = findutils tar make patch perl python3 openssl\n"
        "CriticalPkg = linux dinit lpm\n"
        "\n"
        "# IgnorePkg\n"
        "#   Packages skipped during 'lpm -u' (can still be updated manually).\n"
        "#   Space-separated list. Leave empty to update everything.\n"
        "#\n"
        "IgnorePkg =\n"
        "\n"
        "# LogDir\n"
        "#   Directory for per-package build logs.\n"
        "#   Each package gets its own <pkgname>.log, overwritten each build.\n"
        "#\n"
        "LogDir = /var/log/lpm\n"
        "\n"
        "# FilesDir\n"
        "#   Root directory for the file ownership database.\n"
        "#   Each installed package has a files.list under this directory.\n"
        "#\n"
        "FilesDir = /var/lib/lpm/files\n"
    );

    fclose(f);
    printf("\033[0;36m  ->\033[0m Created default config: %s\n", LPM_CONF_PATH);
}

/* ── lpm_config_load ─────────────────────────────────────────────────────── */

int lpm_config_load(LpmConfig *cfg) {
    set_defaults(cfg);

    FILE *f = fopen(LPM_CONF_PATH, "r");
    if (!f) {
        /* first run — write default config and return defaults */
        lpm_config_write_default();
        return 0;
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *p = trim(line);

        /* skip empty lines and comments */
        if (!*p || *p == '#') continue;

        /* split at '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcasecmp(key, "CriticalPkg") == 0) {
            /* CriticalPkg lines are additive — multiple lines allowed */
            char tokens[CONF_MAX_PKGS][64];
            int n = parse_list(val, tokens, CONF_MAX_PKGS);
            for (int i = 0; i < n && cfg->n_critical < CONF_MAX_PKGS; i++)
                snprintf(cfg->critical_pkgs[cfg->n_critical++], 64,
                         "%s", tokens[i]);

        } else if (strcasecmp(key, "IgnorePkg") == 0) {
            char tokens[CONF_MAX_PKGS][64];
            int n = parse_list(val, tokens, CONF_MAX_PKGS);
            for (int i = 0; i < n && cfg->n_ignore < CONF_MAX_PKGS; i++)
                snprintf(cfg->ignore_pkgs[cfg->n_ignore++], 64,
                         "%s", tokens[i]);

        } else if (strcasecmp(key, "LogDir") == 0) {
            snprintf(cfg->log_dir, sizeof(cfg->log_dir), "%s", val);

        } else if (strcasecmp(key, "FilesDir") == 0) {
            snprintf(cfg->files_dir, sizeof(cfg->files_dir), "%s", val);
        }
        /* unknown keys are silently ignored */
    }

    fclose(f);
    return 0;
}

/* ── lpm_config_is_critical ──────────────────────────────────────────────── */

int lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname) {
    for (int i = 0; i < cfg->n_critical; i++)
        if (strcmp(cfg->critical_pkgs[i], pkgname) == 0) return 1;
    return 0;
}

/* ── lpm_config_is_ignored ───────────────────────────────────────────────── */

int lpm_config_is_ignored(const LpmConfig *cfg, const char *pkgname) {
    for (int i = 0; i < cfg->n_ignore; i++)
        if (strcmp(cfg->ignore_pkgs[i], pkgname) == 0) return 1;
    return 0;
}
