/*
 * config.h — LPM configuration system
 *
 * Parses /etc/lpm/lpm.conf (INI-style, same format as pacman.conf).
 * Config file is MANDATORY — lpm exits if missing, empty, or comment-only.
 * Use 'make install' to deploy the default config.
 *
 * Config file location: /etc/lpm/lpm.conf
 */

#ifndef LPM_CONFIG_H
#define LPM_CONFIG_H

#define LPM_CONF_PATH   "/etc/lpm/lpm.conf"
#define LPM_CONF_DIR    "/etc/lpm"

/* Maximum number of entries in CriticalPkg / IgnorePkg lists */
#define CONF_MAX_PKGS   256

/*
 * Boolean config values — only y/Y/n/N accepted.
 * Anything else is a hard parse error.
 */
typedef enum {
    CONF_BOOL_YES = 1,
    CONF_BOOL_NO  = 0
} ConfBool;

/*
 * LpmConfig — runtime configuration loaded from lpm.conf.
 *
 * CriticalPkg  : packages requiring triple-YES confirmation before removal.
 * IgnorePkg    : packages skipped during 'lpm -u'.
 * log_dir      : per-package build log directory (default: /var/log/lpm).
 * files_dir    : file ownership database root (default: /var/lib/lpm/files).
 * makeflags    : passed to every make invocation (default: -j<ceil(nproc/2)>).
 * default_yes  : DEFAULT_YES=y → skip all confirmations.
 * default_strict: DEFAULT_STRICT=y → treat check() failure as fatal.
 * run_check    : RUN_CHECK=y → auto-run check() after build.
 * strict_build : STRICT_BUILD=y → block install on check() failure.
 */
typedef struct {
    char     critical_pkgs[CONF_MAX_PKGS][64];
    int      n_critical;

    char     ignore_pkgs[CONF_MAX_PKGS][64];
    int      n_ignore;

    char     log_dir[256];
    char     files_dir[256];
    char     makeflags[256];  /* e.g. "-j4" or "-j$(nproc)" */

    ConfBool default_yes;
    ConfBool default_strict;
    ConfBool run_check;
    ConfBool strict_build;
} LpmConfig;

/*
 * lpm_config_load — parse /etc/lpm/lpm.conf into *cfg.
 * Exits with error if:
 *   - file is missing
 *   - file is empty
 *   - file contains only comments / whitespace
 *   - a boolean key has an invalid value (not y/Y/n/N)
 * Returns 0 on success.
 */
int  lpm_config_load(LpmConfig *cfg);

/*
 * lpm_config_write_default — write a default lpm.conf to LPM_CONF_PATH.
 * Called by 'make install' only — never called at runtime.
 */
void lpm_config_write_default(void);

/* Lookup helpers */
int  lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname);
int  lpm_config_is_ignored (const LpmConfig *cfg, const char *pkgname);

#endif /* LPM_CONFIG_H */
