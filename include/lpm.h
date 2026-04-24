#pragma once
#ifndef LPM_H
#define LPM_H

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
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
#define LPM_VERSION "1.2.0-alpha"
#define LPM_LOCK_FILE "/var/lock/lpm.lock"
#define LPM_DB_DIR "/var/lib/lpm/db"
#define LPM_DB "/var/lib/lpm/db/installed"
#define LPM_FILES_DIR "/var/lib/lpm/files"
#define LPM_CACHE_DIR "/var/cache/lpm"
#define LPM_BUILD_DIR "/var/cache/lpm"
#define LPM_CONF_FILE "/etc/lpm/lpm.conf"
#define LPM_PKGBUILD_DIR "/usr/src/lpm"
#define LPM_LOG_FILE "/var/log/lpm/lpm.log"
#define LPM_LOG_DIR "/var/log/lpm"
#define LPM_AUDIT_LOG "/var/log/lpm/audit.log"

/* ── limits ──────────────────────────────────────────────────────────── */
#define LPM_MAX_DEPS 128
#define LPM_MAX_SOURCES 32
#define LPM_MAX_BACKUP 64
#define LPM_MAX_FILES 65536
#define LPM_NAME_MAX 128
#define LPM_VER_MAX 64
#define LPM_URL_MAX 2048
#define LPM_PATH_MAX 512

/* compat aliases — old files use these names */
#define MAX_STR LPM_PATH_MAX
#define MAX_DEPS LPM_MAX_DEPS
#define MAX_SRCS LPM_MAX_SOURCES
#define MAX_CMD 2048

/* ── colors ──────────────────────────────────────────────────────────── */
#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"
#define C_RED "\033[1;31m"
#define C_GREEN "\033[1;32m"
#define C_YELLOW "\033[1;33m"
#define C_BLUE "\033[1;34m"
#define C_PINK "\033[1;35m"
#define C_CYAN "\033[1;36m"
#define C_GRAY "\033[0;90m"

/* ── enums ───────────────────────────────────────────────────────────── */
typedef enum { PKG_TYPE_SOURCE = 0, PKG_TYPE_BINARY = 1 } PkgType;
typedef enum {
  CKSUM_SKIP = 0,
  CKSUM_MD5 = 1,
  CKSUM_SHA256 = 2,
  CKSUM_SHA512 = 3
} CksumType;
typedef enum { REASON_EXPLICIT = 0, REASON_DEP = 1 } InstallReason;
typedef enum {
  PKG_STATE_PENDING = 0,
  PKG_STATE_BUILT = 1,
  PKG_STATE_STAGED = 2,
  PKG_STATE_MERGED = 3,
  PKG_STATE_FAILED = 4
} PkgState;
typedef enum { DL_AUTO = 0, DL_WGET, DL_CURL, DL_NONE } Downloader;

/* ── source entry ────────────────────────────────────────────────────── */
typedef struct {
  char url[LPM_URL_MAX];
  char filename[LPM_NAME_MAX];
  char checksum[129];
  CksumType cksum_type;
} Source;

/* ── Package (new API) ───────────────────────────────────────────────── */
typedef struct Package {
  char name[LPM_NAME_MAX];
  char version[LPM_VER_MAX];
  char release[16];
  char description[512];
  char license[128];
  PkgType type;

  char depends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int ndepends;
  char makedepends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nmakedepends;
  char optdepends[LPM_MAX_DEPS][LPM_NAME_MAX * 2];
  int noptdepends;
  char conflicts[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nconflicts;
  char provides[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nprovides;
  char replaces[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nreplaces;

  Source sources[LPM_MAX_SOURCES];
  int nsources;
  char backup[LPM_MAX_BACKUP][LPM_PATH_MAX];
  int nbackup;

  int has_pre_install, has_post_install;
  int has_pre_remove, has_post_remove;
  int has_check, has_uninstall;

  PkgState state;
  InstallReason reason;
  char pkgbuild_path[LPM_PATH_MAX];
  char pkg_dir[LPM_PATH_MAX];
  char src_dir[LPM_PATH_MAX];
} Package;

/* ── Pkg (compat: dep.c / search.c / pkgbuild.c use this) ───────────── */
typedef struct {
  char pkgname[LPM_NAME_MAX];
  char pkgver[LPM_VER_MAX];
  char pkgrel[16];
  char depends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int ndepends;
  char recommends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nrecommends;
  char makedepends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nmakedepends;
  char source[LPM_MAX_SOURCES][LPM_PATH_MAX];
  int nsources;
  char sha256sums[LPM_MAX_SOURCES][129];
  char md5sums[LPM_MAX_SOURCES][33];
  char pbfile[LPM_PATH_MAX];
  int has_check;
  int has_uninstall;
} Pkg;

/* ── InstalledPkg ────────────────────────────────────────────────────── */
typedef struct {
  char name[LPM_NAME_MAX];
  char version[LPM_VER_MAX];
  char release[16];
  char description[512];
  PkgType type;
  int reason;
  int64_t install_time;
  size_t install_size;
  char files[LPM_MAX_FILES][LPM_PATH_MAX];
  int nfiles;
} InstalledPkg;

/* ── LpmFlags (CLI flags) ────────────────────────────────────────────── */
typedef struct {
  int yes;        /* --yes: skip all confirmation prompts    */
  int strict;     /* --strict: check() failure = hard block  */
  int force;      /* --force: override dep/critical checks   */
  int no_confirm; /* --no-confirm: non-interactive remove    */
} LpmFlags;

/* ── LpmConfig ───────────────────────────────────────────────────────── */
typedef struct {
  /* compiler/linker */
  char cflags[512], cxxflags[512], ldflags[512], makeflags[256];
  char cc[64], cxx[64];
  int jobs;
  /* paths */
  char build_dir[LPM_PATH_MAX];
  char pkg_dest[LPM_PATH_MAX];
  char src_dest[LPM_PATH_MAX];
  char log_dir[256];
  char files_dir[256];
  /* behavior toggles */
  int color, confirm, keep_src, keep_pkg;
  int check_space, parallel_dl, max_dl_threads, verify_sig;
  int default_yes, default_strict, run_check, strict_build;
  /* downloader */
  char downloader[16];
  /* package lists */
  char critical_pkgs[256][64];
  int n_critical;
  char ignore_pkgs[256][64];
  int n_ignore;
} LpmConfig;

/* ── Transaction ─────────────────────────────────────────────────────── */
typedef struct {
  Package **install;
  int ninstall;
  Package **remove;
  int nremove;
  Package **upgrade;
  int nupgrade;
  char (*merged_files)[LPM_PATH_MAX];
  int nmerged;
  int committed;
} Transaction;

/* ── FetchJob ────────────────────────────────────────────────────────── */
typedef struct {
  int slot, total;
  char url[LPM_URL_MAX];
  char dest[LPM_PATH_MAX];
  char filename[LPM_NAME_MAX];
  char checksum[129];
  CksumType cksum_type;
  int result;
} FetchJob;

/* ── globals ─────────────────────────────────────────────────────────── */
extern LpmConfig g_cfg;
extern int g_lock_fd;
extern int g_verbose;

/* ── util.c ──────────────────────────────────────────────────────────── */
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

/* ── config.c ────────────────────────────────────────────────────────── */
int lpm_config_load(const char *path, LpmConfig *cfg);
void lpm_config_defaults(LpmConfig *cfg);
void lpm_config_dump(const LpmConfig *cfg);

/* ── pkgbuild.c (old Pkg API) ────────────────────────────────────────── */
int pkgbuild_parse(const char *pbfile, Pkg *pkg);
int dep_satisfied(const char *spec);
char *reverse_deps(const char *target);

/* ── dep.c ───────────────────────────────────────────────────────────── */
void cmd_deptree(int argc, char **argv);
int dep_resolve_queue(const char *pkgname, char out[][MAX_STR], int maxout);
void dep_set_folder(const char *pkgname, const char *folder);

/* ── download.c ──────────────────────────────────────────────────────── */
Downloader dl_detect(const char *override);
int dl_file(const char *url, const char *dest, const char *filename, int slot,
            int total);
int dl_fetch_all(FetchJob *jobs, int njobs);

/* ── checksum (stub — implement in checksum.c) ───────────────────────── */
int cksum_verify(const char *path, const char *expected, CksumType type);

/* ── build.c ─────────────────────────────────────────────────────────── */
int pkg_build(Package *pkg, const LpmConfig *cfg);
int pkg_run_check(Package *pkg);
int pkg_run_package(Package *pkg);
int pkg_run_hook(const char *hook, Package *pkg);
void cmd_sync(int argc, char **argv);
void cmd_local(int argc, char **argv);
void cmd_fetch(int argc, char **argv);
void cmd_check(int argc, char **argv);
void cmd_remove(int argc, char **argv);
void cmd_update(int argc, char **argv);

/* ── merge.c (stub) ──────────────────────────────────────────────────── */
int pkg_merge(Package *pkg, const char *root, Transaction *tx);

/* ── db.c ────────────────────────────────────────────────────────────── */
int db_is_installed(const char *pkgname);
char *db_get_version(const char *pkgname);
void db_add(const char *pkgname, const char *ver, const char *rel);
void db_remove(const char *pkgname);
void db_files_save(const char *pkgname, const char *pkgdir);
int db_files_remove(const char *pkgname);
int db_init(void);
int db_record_install(const Package *pkg, const char *root);
int db_query(const char *name, InstalledPkg *out);
int db_list_all(InstalledPkg **out, int *count);
int db_query_owner(const char *filepath, char *out_name, size_t sz);
int db_list_files(const char *name);
int db_check_integrity(const char *name);

/* ── transaction.c ───────────────────────────────────────────────────── */
Transaction *tx_new(void);
void tx_free(Transaction *tx);
int tx_add_install(Transaction *tx, Package *pkg);
int tx_add_remove(Transaction *tx, Package *pkg);
int tx_commit(Transaction *tx, const char *root);
int tx_rollback(Transaction *tx, const char *root);

/* ── safety.c ────────────────────────────────────────────────────────── */
int lpm_lock_acquire(void);
void lpm_lock_release(void);
int safety_check_conflicts(Package **pkgs, int n, const char *root);
int safety_check_file_conflicts(const char *pkgdir, const char *pkgname,
                                int force);
int safety_check_toolchain(const char *pkgdir, const char *pkgname);
int safety_check_space(Package **pkgs, int n, const char *root);
int safety_backup_configs(Package *pkg, const char *root);
int safety_protect_configs(Package *pkg, const char *pkgdir, const char *root);
int safety_restore_configs(Package *pkg, const char *root);
int safety_guard_symlinks(const char *src_path, const char *dest_path);

/* ── search.c ────────────────────────────────────────────────────────── */
void cmd_search(int argc, char **argv);
void cmd_info(int argc, char **argv);
void cmd_list(int argc, char **argv);
void cmd_orphans(int argc, char **argv);

/* ── cache.c ─────────────────────────────────────────────────────────── */
void cmd_rcc(int argc, char **argv);

/* ── build.c extras ──────────────────────────────────────────────────── */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs,
                    int maxpkgs);
void pkg_log_path(const char *pkgname, char *out, size_t outsz);

/* ── config.c extras ─────────────────────────────────────────────────── */
int lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname);
int lpm_config_is_ignored(const LpmConfig *cfg, const char *pkgname);

#endif /* LPM_H */
