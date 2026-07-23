/*
 * config.h — LPM configuration notes
 *
 * Parses /etc/lpm/lpm.conf (key = value).
 * Canonical LpmConfig and parser prototypes live in lpm.h.
 *
 * Config file location: /etc/lpm/lpm.conf
 *
 * Notable options:
 *   CriticalPkg  — packages that require --force to remove
 *   IgnorePkg    — packages skipped during system upgrades
 *   LOGDIR       — per-package build log directory
 *   FILESDIR     — file ownership database root
 *   MAKEFLAGS    — extra flags passed to make
 *   JOBS         — parallel build jobs (0 = automatic)
 *
 * Legacy aliases LogDir / FilesDir are still accepted.
 */

#ifndef LPM_CONFIG_H
#define LPM_CONFIG_H

#define LPM_CONF_PATH   "/etc/lpm/lpm.conf"
#define LPM_CONF_DIR    "/etc/lpm"
#define CONF_MAX_PKGS   256

#endif /* LPM_CONFIG_H */
