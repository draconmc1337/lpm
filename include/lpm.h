#pragma once
#ifndef LPM_H
#define LPM_H

/* Feature test macros — must come before ANY system header */
#ifndef _XOPEN_SOURCE
#  define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── version ─────────────────────────────────────────────────────────── */
#define LPM_VERSION "1.5.0"

/* ── upstream repository ────────────────────────────────────────────────
 * Single source of truth for the repo-lotus base URL.
 */
#define REPO_BASE "https://raw.githubusercontent.com/draconmc1337/repo-lotus/main"

#define LPM_LOCK_FILE "/var/lock/lpm.lock"
#define LPM_DB_DIR "/var/lib/lpm/db"
#define LPM_DB "/var/lib/lpm/db/installed"
#define LPM_FILES_DIR "/var/lib/lpm/files"
#define LPM_CACHE_DIR "/var/cache/lpm"
#define LPM_BUILD_DIR "/var/cache/lpm"
#define LPM_CONF_FILE "/etc/lpm/lpm.conf"
#include "llpm/handle.h"
#define LPM_PKGBUILD_DIR LLPM_PKGBUILD_DIR
#define LPM_BUILD_META_DIR "/var/lib/lpm/buildmeta"


#define LPM_LOG_FILE "/var/log/lpm/lpm.log"
#define LPM_LOG_DIR "/var/log/lpm"
#define LPM_AUDIT_LOG "/var/log/lpm/audit.log"

/* ── limits ─────────────────────────────────────────────────────────── */
#define LPM_MAX_DEPS 128
#define LPM_MAX_SOURCES 32
#define LPM_MAX_BACKUP 64
#define LPM_MAX_FILES 65536
#define LPM_NAME_MAX 128
#define LPM_VER_MAX 64
#define LPM_URL_MAX 4096
#define LPM_PATH_MAX 4096

/* short-form aliases */
#define MAX_STR LPM_PATH_MAX
#define MAX_DEPS LPM_MAX_DEPS
#define MAX_SRCS LPM_MAX_SOURCES
#define MAX_CMD 16384

/* ── colors ─────────────────────────────────────────────────────────── */
#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_RED    "\033[1;31m"
#define C_GREEN  "\033[1;32m"
#define C_YELLOW "\033[1;33m"
#define C_BLUE   "\033[1;34m"
#define C_PINK   "\033[1;35m"
#define C_CYAN   "\033[1;36m"
#define C_GRAY   "\033[0;90m"

/* ── debug level (global) ───────────────────────────────────────────── */
extern int g_debug;

/* DBG/TRACE macros */
#define DBG(level, fmt, ...) \
    do { \
        if (g_debug >= (level)) { \
            if ((level) >= 3) \
                fprintf(stderr, "\033[2m[TRACE] " fmt "\033[0m\n", ##__VA_ARGS__); \
            else \
                fprintf(stderr, "\033[2m[DEBUG] " fmt "\033[0m\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define TRACE(fmt, ...) \
    DBG(3, "%s(): " fmt, __func__, ##__VA_ARGS__)

/* ── enums ─────────────────────────────────────────────────────────── */
typedef enum { PKG_TYPE_SOURCE = 0, PKG_TYPE_BINARY = 1 } PkgType;
typedef enum { CKSUM_SKIP = 0, CKSUM_MD5 = 1, CKSUM_SHA256 = 2, CKSUM_SHA512 = 3 } CksumType;
typedef enum { REASON_EXPLICIT = 0, REASON_DEP = 1 } InstallReason;
typedef enum { PKG_STATE_PENDING = 0, PKG_STATE_BUILT = 1, PKG_STATE_STAGED = 2, PKG_STATE_MERGED = 3, PKG_STATE_FAILED = 4 } PkgState;
typedef enum { DL_AUTO = 0, DL_WGET, DL_CURL, DL_NONE } Downloader;

/* ── Internal error codes (used only inside sync subsystem) ───────────── */
#define ERR_NET_DNS           0x1001
#define ERR_NET_CONNREFUSED   0x1002
#define ERR_NET_TIMEOUT       0x1003
#define ERR_NET_TLS           0x1004
#define ERR_HTTP_404          0x1005
#define ERR_HTTP_403          0x1006
#define ERR_HTTP_429          0x1007
#define ERR_HTTP_500          0x1008
#define ERR_HTTP_502          0x1009
#define ERR_HTTP_503          0x100A
#define ERR_NET_REDIRECT_LOOP 0x100B
#define ERR_DOWNLOADER_MISSING 0x100C
#define ERR_NET_CONNRESET     0x100D

#define ERR_REPO_EMPTY        0x2001
#define ERR_REPO_INVALID      0x2002
#define ERR_REPO_HTML         0x2003
#define ERR_REPO_DUPPKG       0x2004
#define ERR_REPO_BADMETA      0x2005
#define ERR_REPO_CKSUM        0x2006
#define ERR_REPO_SIGFAILED    0x2007
#define ERR_REPO_UNSUPPVER    0x2008

/* ── source entry ───────────────────────────────────────────────────── */
typedef struct {
  char url[LPM_URL_MAX];
  char filename[LPM_NAME_MAX];
  char checksum[200];
  CksumType cksum_type;
} Source;

/* ... rest of header unchanged (typedefs etc) ... */

/* keep the rest of the existing declarations intact */

/* util.c */
void die(const char *fmt, ...);
void warn(const char *fmt, ...);
void lpm_log(const char *fmt, ...);
void lpm_audit(const char *fmt, ...);
int confirm(const char *prompt);
int confirm_word(const char *prompt, const char *word);
void init_dirs(void);
void check_root(void);
int version_compare(const char *a, const char *b);
int version_gte(const char *have, const char *need);
int util_run(const char *cmd);
int util_run_env(const char *cmd, char *const envp[]);
char *util_strip(char *s);
int util_mkdirp(const char *path, mode_t mode);
int util_rmrf(const char *path);
int util_copy_file(const char *src, const char *dst);
long util_disk_free(const char *path);
int util_nproc(void);
void util_progress_bar(int slot, int total, const char *name, int percent,
                       int done, int failed);

/* config.c */
int lpm_config_load(const char *path, void *cfg);
void lpm_config_defaults(void *cfg);
void lpm_config_dump(const void *cfg);

/* download.c */
Downloader dl_detect(const char *override);
int dl_file(const char *url, const char *dest, const char *filename, int slot,
            int total);
int dl_fetch_all(void *jobs, int njobs);

#endif /* LPM_H */
