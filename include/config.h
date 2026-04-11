/*
 * config.h — LPM configuration system
 *
 * Parses /etc/lpm/lpm.conf (INI-style, same format as pacman.conf).
 * All fields have safe defaults so lpm works even without a config file.
 *
 * Config file location: /etc/lpm/lpm.conf
 * Default config written on first run if missing.
 */

#ifndef LPM_CONFIG_H
#define LPM_CONFIG_H

#define LPM_CONF_PATH   "/etc/lpm/lpm.conf"
#define LPM_CONF_DIR    "/etc/lpm"

/* Maximum number of entries in CriticalPkg / IgnorePkg lists */
#define CONF_MAX_PKGS   256

/*
 * LpmConfig — runtime configuration loaded from lpm.conf.
 *
 * CriticalPkg : packages that trigger the full 3-step removal confirmation.
 *               Equivalent to pacman's HoldPkg but with tiered UX.
 * IgnorePkg   : packages skipped during `lpm -u` (still installable manually).
 * log_dir     : directory for per-package build logs (default: /var/log/lpm).
 * files_dir   : file ownership database root (default: /var/lib/lpm/files).
 */
typedef struct {
    char  critical_pkgs[CONF_MAX_PKGS][64]; /* CriticalPkg list */
    int   n_critical;

    char  ignore_pkgs[CONF_MAX_PKGS][64];   /* IgnorePkg list   */
    int   n_ignore;

    char  log_dir[256];                     /* LogDir           */
    char  files_dir[256];                   /* FilesDir         */
} LpmConfig;

/*
 * lpm_config_load — parse /etc/lpm/lpm.conf into *cfg.
 * If the file does not exist, cfg is filled with defaults and
 * lpm_config_write_default() is called to create it.
 * Returns 0 on success, -1 on unrecoverable error.
 */
int  lpm_config_load(LpmConfig *cfg);

/*
 * lpm_config_write_default — write a default lpm.conf to LPM_CONF_PATH.
 * Called automatically by lpm_config_load() on first run.
 */
void lpm_config_write_default(void);

/*
 * lpm_config_is_critical — return 1 if pkgname is in cfg->critical_pkgs.
 */
int  lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname);

/*
 * lpm_config_is_ignored — return 1 if pkgname is in cfg->ignore_pkgs.
 * Used by cmd_update() to skip ignored packages.
 */
int  lpm_config_is_ignored(const LpmConfig *cfg, const char *pkgname);

#endif /* LPM_CONFIG_H */
