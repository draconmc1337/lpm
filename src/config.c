/*
 * config.c — LPM configuration parser (v1.1.5-alpha)
 *
 * Config file: /etc/lpm/lpm.conf  (mandatory — lpm exits if missing/empty)
 *
 * Supported keys:
 *
 *   CriticalPkg  = glibc gcc bash ...   (additive, space-separated)
 *   IgnorePkg    = linux-headers ...    (additive, space-separated)
 *   LogDir       = /var/log/lpm
 *   FilesDir     = /var/lib/lpm/files
 *   MAKEFLAGS    = -j4
 *   DEFAULT_YES  = y | n
 *   DEFAULT_STRICT = y | n
 *   RUN_CHECK    = y | n
 *   STRICT_BUILD = y | n
 *
 * Boolean values: only y / Y / n / N accepted — anything else is a hard error.
 * Lines starting with '#' are comments.
 * Unknown keys are silently ignored for forward compatibility.
 */

#include "config.h"
#include "lpm.h"
#include <ctype.h>
#include <unistd.h>

/* ── internal helpers ────────────────────────────────────────────────────── */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

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

/*
 * parse_bool — parse a boolean config value.
 * Accepts only: y Y n N
 * Anything else → print error and exit(1).
 */
static ConfBool parse_bool(const char *key, const char *val) {
    if (strcmp(val, "y") == 0 || strcmp(val, "Y") == 0) return CONF_BOOL_YES;
    if (strcmp(val, "n") == 0 || strcmp(val, "N") == 0) return CONF_BOOL_NO;
    fprintf(stderr,
        "\033[0;31merror:\033[0m invalid value for %s: '%s'\n"
        "  Only 'y' or 'n' are accepted.\n", key, val);
    exit(1);
}

/*
 * default_makeflags — compute -j<ceil(nproc/2)> as fallback.
 * 1 core → -j1, 3 cores → -j2, 8 cores → -j4, 9 cores → -j5
 */
static void default_makeflags(char *out, size_t outsz) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 1;
    long jobs = (nproc + 1) / 2;   /* ceil(nproc / 2) */
    snprintf(out, outsz, "-j%ld", jobs);
}

/* ── defaults ────────────────────────────────────────────────────────────── */

static void set_defaults(LpmConfig *cfg) {
    cfg->n_critical    = 0;
    cfg->n_ignore      = 0;
    cfg->default_yes   = CONF_BOOL_NO;
    cfg->default_strict= CONF_BOOL_NO;
    cfg->run_check     = CONF_BOOL_NO;
    cfg->strict_build  = CONF_BOOL_NO;
    snprintf(cfg->log_dir,   sizeof(cfg->log_dir),   "%s", LPM_LOG_DIR);
    snprintf(cfg->files_dir, sizeof(cfg->files_dir), "%s", LPM_FILES_DIR);
    default_makeflags(cfg->makeflags, sizeof(cfg->makeflags));
}

/* ── lpm_config_write_default ────────────────────────────────────────────── */

void lpm_config_write_default(void) {
    mkdir(LPM_CONF_DIR, 0755);

    FILE *f = fopen(LPM_CONF_PATH, "w");
    if (!f) {
        fprintf(stderr,
            "\033[1;33mwarning:\033[0m could not write default config to %s\n",
            LPM_CONF_PATH);
        return;
    }

    /* compute default MAKEFLAGS for the comment */
    char mf[32];
    default_makeflags(mf, sizeof(mf));

    fprintf(f,
        "#\n"
        "# /etc/lpm/lpm.conf — Lotus Package Manager configuration\n"
        "#\n"
        "\n"
        "# CriticalPkg\n"
        "#   Packages requiring triple-YES confirmation before removal.\n"
        "#   Without --force these are hard-blocked.\n"
        "#   Additive — multiple lines are merged.\n"
        "#\n"
        "CriticalPkg = glibc gcc binutils bash coreutils sed grep gawk\n"
        "CriticalPkg = findutils tar make patch perl python3 openssl\n"
        "CriticalPkg = linux dinit lpm\n"
        "\n"
        "# IgnorePkg\n"
        "#   Packages skipped during 'lpm -u'.\n"
        "#   Can still be updated manually with 'lpm -u <pkg>'.\n"
        "#\n"
        "IgnorePkg =\n"
        "\n"
        "# MAKEFLAGS\n"
        "#   Passed to every make invocation during build.\n"
        "#   Default: ceil(nproc/2) — conservative to avoid OOM on large builds.\n"
        "#   Set to -j$(nproc) for maximum speed on machines with enough RAM.\n"
        "#\n"
        "MAKEFLAGS = %s\n"
        "\n"
        "# DEFAULT_YES\n"
        "#   y = skip all confirmation prompts (automation/scripting).\n"
        "#   n = always ask (default, interactive use).\n"
        "#\n"
        "DEFAULT_YES = n\n"
        "\n"
        "# DEFAULT_STRICT\n"
        "#   y = treat check() failure as a fatal install block globally.\n"
        "#   n = warn and ask (default).\n"
        "#\n"
        "DEFAULT_STRICT = n\n"
        "\n"
        "# RUN_CHECK\n"
        "#   y = automatically run check() after build without asking.\n"
        "#   n = ask each time (default).\n"
        "#\n"
        "RUN_CHECK = n\n"
        "\n"
        "# STRICT_BUILD\n"
        "#   y = check() failure blocks install (same as --strict flag).\n"
        "#   n = warn only (default).\n"
        "#\n"
        "STRICT_BUILD = n\n"
        "\n"
        "# LogDir\n"
        "#   Per-package build log directory.\n"
        "#\n"
        "LogDir = /var/log/lpm\n"
        "\n"
        "# FilesDir\n"
        "#   File ownership database root.\n"
        "#\n"
        "FilesDir = /var/lib/lpm/files\n",
        mf);

    fclose(f);
    printf("\033[0;36m  ->\033[0m Installed default config: %s\n", LPM_CONF_PATH);
}

/* ── lpm_config_load ─────────────────────────────────────────────────────── */

int lpm_config_load(LpmConfig *cfg) {
    set_defaults(cfg);

    FILE *f = fopen(LPM_CONF_PATH, "r");
    if (!f) {
        fprintf(stderr,
            "\033[0;31merror:\033[0m config not found: %s\n"
            "Run 'make install' to create the default config.\n",
            LPM_CONF_PATH);
        exit(1);
    }

    /* reject empty file */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz == 0) {
        fclose(f);
        fprintf(stderr,
            "\033[0;31merror:\033[0m config is empty: %s\n"
            "Restore it or run 'make install' to reset to defaults.\n",
            LPM_CONF_PATH);
        exit(1);
    }

    /* reject comment-only / whitespace-only file */
    int has_real_content = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p && *p != '#') { has_real_content = 1; break; }
    }
    if (!has_real_content) {
        fclose(f);
        fprintf(stderr,
            "\033[0;31merror:\033[0m config has no active keys: %s\n"
            "Restore it or run 'make install' to reset to defaults.\n",
            LPM_CONF_PATH);
        exit(1);
    }
    rewind(f);

    /* parse */
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *p = trim(line);
        if (!*p || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcasecmp(key, "CriticalPkg") == 0) {
            char tokens[CONF_MAX_PKGS][64];
            int n = parse_list(val, tokens, CONF_MAX_PKGS);
            for (int i = 0; i < n && cfg->n_critical < CONF_MAX_PKGS; i++)
                snprintf(cfg->critical_pkgs[cfg->n_critical++], 64, "%s", tokens[i]);

        } else if (strcasecmp(key, "IgnorePkg") == 0) {
            char tokens[CONF_MAX_PKGS][64];
            int n = parse_list(val, tokens, CONF_MAX_PKGS);
            for (int i = 0; i < n && cfg->n_ignore < CONF_MAX_PKGS; i++)
                snprintf(cfg->ignore_pkgs[cfg->n_ignore++], 64, "%s", tokens[i]);

        } else if (strcasecmp(key, "LogDir") == 0) {
            snprintf(cfg->log_dir, sizeof(cfg->log_dir), "%s", val);

        } else if (strcasecmp(key, "FilesDir") == 0) {
            snprintf(cfg->files_dir, sizeof(cfg->files_dir), "%s", val);

        } else if (strcasecmp(key, "MAKEFLAGS") == 0) {
            snprintf(cfg->makeflags, sizeof(cfg->makeflags), "%s", val);

        } else if (strcasecmp(key, "DEFAULT_YES") == 0) {
            cfg->default_yes = parse_bool("DEFAULT_YES", val);

        } else if (strcasecmp(key, "DEFAULT_STRICT") == 0) {
            cfg->default_strict = parse_bool("DEFAULT_STRICT", val);

        } else if (strcasecmp(key, "RUN_CHECK") == 0) {
            cfg->run_check = parse_bool("RUN_CHECK", val);

        } else if (strcasecmp(key, "STRICT_BUILD") == 0) {
            cfg->strict_build = parse_bool("STRICT_BUILD", val);
        }
        /* unknown keys silently ignored */
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
