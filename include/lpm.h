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
#define LPM_VERSION "2.0.0"

/* ── upstream repository ────────────────────────────────────────────────
 * Single source of truth for the repo-lotus base URL. build.c, sync.c and
 * dep.c all fetch from this — previously each file redefined REPO_BASE
 * independently, and all three still pointed at the archived
 * lotus-repository repo after the migration to repo-lotus. Do not
 * redefine REPO_BASE anywhere else.
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

/* ── limits ──────────────────────────────────────────────────────────── */
#define LPM_MAX_DEPS 128
#define LPM_MAX_SOURCES 32
#define LPM_MAX_BACKUP 64
#define LPM_MAX_FILES 65536
#define LPM_NAME_MAX 128
#define LPM_VER_MAX 64
#define LPM_URL_MAX 4096
#define LPM_PATH_MAX 4096

/* short-form aliases — used throughout src/ (80+ call sites) in preference
 * to the LPM_-prefixed names below. Single source of truth, not a
 * compat/legacy shim: MAX_STR is #define'd directly from LPM_PATH_MAX so
 * the two can never drift apart. */
#define MAX_STR LPM_PATH_MAX
#define MAX_DEPS LPM_MAX_DEPS
#define MAX_SRCS LPM_MAX_SOURCES
#define MAX_CMD 16384

/* ── colors (disabled — monochrome professional output) ─────────────── */
#define C_RESET  ""
#define C_BOLD   ""
#define C_RED    ""
#define C_GREEN  ""
#define C_YELLOW ""
#define C_BLUE   ""
#define C_PINK   ""
#define C_CYAN   ""
#define C_GRAY   ""

/* ── debug level (global, set from --debug=N or LPM_DEBUG env) ──────── */
extern int g_debug;

/* DBG(level, fmt, ...) — prints to stderr when g_debug >= level
 *   level 1  [DEBUG]  user-facing: dep resolution, cache hit/miss, mirror
 *   level 2  [DEBUG]  deep:        URLs, checksums, dep graph edges
 *   level 3  [TRACE]  dev-only:    function entry/exit, db lookups
 */
#define DBG(level, fmt, ...) \
    do { \
        if (g_debug >= (level)) { \
            if ((level) >= 3) \
                fprintf(stderr, "[TRACE] " fmt "\n", ##__VA_ARGS__); \
            else \
                fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

#define TRACE(fmt, ...) \
    DBG(3, "%s(): " fmt, __func__, ##__VA_ARGS__)

/* ── enums ───────────────────────────────────────────────────────────── */
typedef enum { PKG_TYPE_SOURCE = 0, PKG_TYPE_BINARY = 1 } PkgType;
typedef enum {
  CKSUM_INVALID = -1, /* spec violation: not sha512:/sha256:/md5:/SKIP —
                        * must be rejected, never silently treated as
                        * CKSUM_SKIP (that would skip verification). */
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

/* ── Package — the one package representation ──────────────────────────
 * lpm 2.0: previously there were two parallel structs (Package, used by
 * db.c/merge.c/safety.c/sync.c/transaction.c, and Pkg, used by
 * dep.c/search.c/pkgbuild.c/pkgbuild_parser.c/verify.c/build.c), plus a
 * third near-duplicate (PkgMeta) as the on-disk parser cache format.
 * build.c bridged Package↔Pkg by hand for every package it touched.
 * Both LPDF parsers (parse_pkgbuild_c in pkgbuild_parser.c, and the bash
 * fallback pkgbuild_parse() in pkgbuild.c) now fill this struct directly.
 * There is no conversion layer and no second package representation
 * anywhere in the codebase.
 *
 * Fields dropped in the merge (were declared, never consulted anywhere):
 *   optdepends/noptdepends, has_build, has_package, has_remove,
 *   has_uninstall, has_pre_remove, has_post_remove. check()/build()/
 *   package() are already fully executed inline in do_build_install()
 *   (build.c) gated on has_check alone — they don't need their own
 *   has_* flags. pre_remove/post_remove have no consumer because package
 *   removal doesn't go through a hook-firing path yet (tx_commit() only
 *   processes tx->install, not tx->remove) — add has_pre_remove /
 *   has_post_remove back together with that, not before it exists. */
/* ── source entry ────────────────────────────────────────────────────── */
typedef struct {
  char url[LPM_URL_MAX];
  char filename[LPM_NAME_MAX];
  /* unified: "sha512:hex", "sha256:hex", "md5:hex", "SKIP", or "" */
  char checksum[200];
  CksumType cksum_type;  /* derived from checksum prefix at parse time */
} Source;

typedef struct Package {
  char name[LPM_NAME_MAX];
  char version[LPM_VER_MAX];
  char release[16];
  char description[512];
  char license[128];
  PkgType type;

  char depends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int ndepends;
  char recommends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nrecommends;
  char makedepends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int nmakedepends;
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
  char groups[LPM_MAX_DEPS][LPM_NAME_MAX]; /* package groups e.g. "xlib" */
  int ngroups;
  long dl_size;    /* download size in bytes, binary packages only */
  long inst_size;  /* installed size in bytes, binary packages only */

  /* has_pre_install/has_post_install: detected from pre_install()/
   * post_install() functions in the LPDF file, run by pkg_run_hook()
   * from tx_commit() (transaction.c) during install.
   * has_check: detected from check(), run inline in do_build_install(). */
  int has_pre_install, has_post_install;
  int has_check;

  PkgState state;
  InstallReason reason;
  char pkgbuild_path[LPM_PATH_MAX];
  char pkg_dir[LPM_PATH_MAX];
  char src_dir[LPM_PATH_MAX];
} Package;

/* ── RepoEntry — one parsed repo.db line ────────────────────────────────
 * Shared by sync.c (builds it), search.c and build.c (consume it) — see
 * parse_repo_db() below. Previously sync.c, search.c, and build.c (x2,
 * pkg_locate_from_db()/repo_db_get_size()) each independently re-parsed
 * the same "pkgname=VER-REL pkgtype=... dlsize=... instsize=... desc=..."
 * line format by hand. One parser now; everyone else just scans the
 * array it returns. */
typedef struct {
    char name[LPM_NAME_MAX];
    char version[LPM_VER_MAX + 16]; /* "ver-rel" */
    char repo[16];                   /* "base" / "extra" / "lotus" */
    int  is_binary;                  /* 1=binary, 0=source */
    long dl_size;                    /* bytes; 0 = unknown */
    long inst_size;                  /* bytes; 0 = source/unknown */
    char desc[256];                  /* short description */
    char provides[LPM_MAX_DEPS][LPM_NAME_MAX];
    int  nprovides;
} RepoEntry;

/* Parse a repo.db file (path) into out[] (caller-allocated, maxn entries).
 * reponame is stamped into each entry's .repo field ("base"/"extra"/
 * "lotus"). Returns the number of entries parsed (0 if the file doesn't
 * exist or is empty — not an error, just "nothing synced yet"). */
int parse_repo_db(const char *path, const char *reponame,
                   RepoEntry *out, int maxn);

/* ── InstalledPkg ────────────────────────────────────────────────────── */
/* NOTE: files are stored on disk at LPM_FILES_DIR/<name>/files.list and
 * accessed via db_list_files() / db_files_remove() / db_check_integrity().
 * They are NOT embedded here — files[LPM_MAX_FILES][LPM_PATH_MAX] would be
 * 256 MB per instance, causing stack overflows and heap exhaustion. */
typedef struct {
  char name[LPM_NAME_MAX];
  char version[LPM_VER_MAX];
  char release[16];
  char description[512];
  PkgType type;
  int reason;
  int64_t install_time;
  size_t install_size;
} InstalledPkg;

/* ── LpmFlags (CLI flags) ────────────────────────────────────────────── *
 * All runtime behavior is controlled here, NOT in lpm.conf.            *
 * --no-confirm     skip all yes/no prompts                             *
 * --strict         treat check() failure as fatal error                *
 * --no-recommended skip recommend prompt entirely                      *
 * --no-check       skip check() phase even if PKGBUILD has it          *
 * --force          override dep/critical/conflict checks               *
 * --debug=N        enable debug output (level 1/2/3)                   *
 * ─────────────────────────────────────────────────────────────────────── */
typedef struct {
  int no_confirm;     /* --no-confirm          */
  int strict;         /* --strict              */
  int no_recommended; /* --no-recommended      */
  int no_check;       /* --no-check            */
  int dry_run;        /* --dry-run             */
  int force;          /* --force               */
  int recursive;      /* -Rs / --recursive     */
  int debug;          /* --debug=N  (1/2/3)    */
  int pack_only;
} LpmFlags;

/* ── Build metadata (reproducible build info) ────────────────────── */
typedef struct {
    char pkgname[LPM_NAME_MAX];
    char pkgver[LPM_VER_MAX];
    char pkgrel[16];
    char built_on[64];         /* "Lotus Linux 2.0"          */
    char compiler[64];         /* "clang 19.1.7"             */
    char libc[32];             /* "musl 1.2.5"               */
    char build_flags[256];     /* CFLAGS used                */
    char build_hash[65];       /* SHA-256 of pkgdir tree     */
    char build_date[32];       /* ISO-8601                   */
    int  is_binary;            /* 1 = pre-built binary pkg   */
} BuildMeta;

/* ── Dry-run: what a transaction WOULD do ────────────────────────── */
typedef enum { DRY_INSTALL = 0, DRY_UPGRADE, DRY_REMOVE } DryOpType;

typedef struct {
    DryOpType type;
    char name[LPM_NAME_MAX];
    char from_ver[LPM_VER_MAX + 16];
    char to_ver[LPM_VER_MAX + 16];
    long dl_bytes;
    long inst_bytes;
    char conflict_with[LPM_NAME_MAX];
    char hook[LPM_NAME_MAX];
} DryOp;

typedef struct {
    DryOp ops[256];
    int   nops;
    long  total_dl;
    long  total_inst_delta;
} DryRun;

/* ── buildmeta / dryrun API ─────────────────────────────────────── */
int  buildmeta_save(const char *pkgname, const BuildMeta *m);
int  buildmeta_load(const char *pkgname, BuildMeta *m);
void buildmeta_collect(const char *pkgname, const char *pkgver,
                       const char *pkgrel, int is_binary,
                       const char *pkgdir, BuildMeta *m);
void dryrun_print(const DryRun *dr);
int  dryrun_build(char **pkgnames, int npkgs, DryRun *dr);
int  dryrun_remove(char **pkgnames, int npkgs, DryRun *dr);

/* ── LpmConfig ───────────────────────────────────────────────────────── *
 * Loaded from /etc/lpm/lpm.conf.                                        *
 * Only build environment, paths, and package lists live here.           *
 * Runtime behavior (confirm, strict, check...) is controlled via        *
 * CLI flags (LpmFlags), not config file.                                *
 * ─────────────────────────────────────────────────────────────────────── */
typedef struct {
  /* compiler / linker */
  char cflags[512];
  char cxxflags[512];
  char ldflags[512];
  char makeflags[256];
  char cc[64];
  char cxx[64];
  int jobs;

  /* paths */
  char build_dir[LPM_PATH_MAX];
  char pkg_dest[LPM_PATH_MAX];
  char src_dest[LPM_PATH_MAX];
  char log_dir[256];
  char files_dir[256];

  /* output */
  int color;

  /* source / download */
  int parallel_dl;
  int max_dl_threads;
  int verify_sig;
  char downloader[16];
  char profile[64];

  /* build behaviour */
  int keep_src;       /* keep extracted source tree after build  */
  int keep_pkg;       /* keep stripped pkg dir after install     */
  int check_space;    /* warn when disk space is low             */
  int confirm;        /* require confirmation before actions      */

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

/* ── on-disk parser cache ────────────────────────────────────────────
 * MetaCacheEntry is a serialization envelope, not a second package
 * representation: magic/version/mtime header for cache-invalidation,
 * wrapping exactly one Package. pkgbuild_parse_fast() (pkgbuild_parser.c)
 * fills the Package directly; there is no separate PkgMeta struct with
 * its own copy of every field. */
#define LPM_META_CACHE_DIR "/var/lib/lpm/cache"
#define LPM_META_MAGIC   0x4C504D43
#define LPM_META_VERSION 7  /* bumped: PkgMeta replaced by MetaCacheEntry
                             * wrapping Package — old .meta cache files
                             * (different layout, different size) are
                             * correctly rejected and re-parsed, see
                             * meta_cache_read()'s version check.        */

typedef struct {
  uint32_t magic;
  uint8_t  version;
  time_t   pkgbuild_mtime;
  Package  pkg;
} MetaCacheEntry;

/* ── globals ─────────────────────────────────────────────────────────── */
extern LpmConfig g_cfg;
extern int g_lock_fd;
extern int g_verbose;
extern int g_debug;
extern volatile sig_atomic_t g_cancel;

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

/* ── pkgbuild_parser.c (fast C parser + binary cache) ───────────────── */
int pkgbuild_parse_fast(const char *pbfile, Package *pkg);
void pkgbuild_invalidate_cache(const char *pkgname);

/* ── pkgbuild.c (bash fallback parser — used when the fast C parser
 * can't handle a PKGBUILD; also fills Package directly) ─────────────── */
int pkgbuild_parse(const char *pbfile, Package *pkg);
int dep_satisfied(const char *spec);
char *reverse_deps(const char *target);

/* ── dep.c ───────────────────────────────────────────────────────────── */
void cmd_deptree(int argc, char **argv);
/* build_all=0: -S mode (skip installed)
 * build_all=1: package-build mode (collect all, including installed) */
int dep_resolve_queue(const char *pkgname, char out[][MAX_STR], int maxout,
                      int build_all);
/* Collect N packages + toposort once — replaces loop of dep_resolve_queue()×N */
int dep_resolve_queue_multi(char **pkgnames, int npkgs, char out[][MAX_STR],
                            int maxout, int build_all);
/* Count + print unresolvable deps from last dep_resolve_queue* call. Returns 0 if all deps found. */
int dep_missing_count(void);
/* Invalidate in-process PkgMeta cache (call after lpm update) */
void dep_meta_cache_invalidate(void);
void dep_set_folder(const char *pkgname, const char *folder);
int dep_get_recommends(const char *pkgname, char out[][MAX_STR], int maxout);

/* ── download.c ──────────────────────────────────────────────────────── */
Downloader dl_detect(const char *override);
int dl_file(const char *url, const char *dest, const char *filename, int slot,
            int total);
int dl_fetch_all(FetchJob *jobs, int njobs);

/* ── checksum.c ──────────────────────────────────────────────────────── */
CksumType checksum_parse_unified(const char *spec,
                                  char *hash_out, size_t hash_outsz);
int cksum_verify(const char *path, const char *expected, CksumType type);

/* ── sha256.c ────────────────────────────────────────────────────────── */
int sha256_file(const char *path, char out_hex[65]);

/* ── build.c ─────────────────────────────────────────────────────────── */
int pkg_build(Package *pkg, const LpmConfig *cfg);
/* Runs the named hook function (pre_install/post_install) from the LPDF
 * file into root. Caller must check has_pre_install/has_post_install
 * first. pkg_run_check()/pkg_run_package() were removed — check() and
 * package() are already run inline in do_build_install(), and the
 * Package-API stubs of the same name had zero callers. */
int pkg_run_hook(const char *hook, Package *pkg, const char *root);
/* build.c — fetch a single PKGBUILD from the remote repo into LPM_PKGBUILD_DIR */
int fetch_pkgbuild(const char *name);
int fetch_all_sources(char queue[][MAX_STR], int nqueue);
void do_build_install(Package *pkg, const char *pbfile_orig, LpmConfig *cfg, int qi, int nqueue, const LpmFlags *flags);
/* pkg_patch_from_fast() removed: it existed to patch up incomplete results
 * from the old bash metadata parser using the fast C parser's output.
 * Now there is one parser (pkgbuild_parse_fast(), pkgbuild_parser.c) and
 * every caller uses it directly — nothing left to patch. */

void cmd_sync(int argc, char **argv);
void cmd_bootstrap(int argc, char **argv);
void cmd_local(int argc, char **argv);
void cmd_fetch(int argc, char **argv);
void cmd_check(int argc, char **argv);    /* lpm test <pkg> — runs PKGBUILD check() */
void cmd_verify(int argc, char **argv);   /* lpm verify [pkg...] — system integrity check */
void cmd_remove(int argc, char **argv);
void cmd_update(int argc, char **argv);
void cmd_suy(int argc, char **argv);      /* lpm upgrade — check + apply updates */
void cmd_db_update(int argc, char **argv); /* lpm update — sync repo databases only */

/* ── merge.c ─────────────────────────────────────────────────────────── */
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
void tx_record_file(Transaction *tx, const char *path);

/* ── safety.c ────────────────────────────────────────────────────────── */
int lpm_lock_acquire(void);
void lpm_lock_release(void);
int  lpm_sig_verify(const char *filepath, const char *sigpath, int sig_required);
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
int  cmd_group_expand(const char *group_name,
                      char out[][LPM_NAME_MAX], int max_out);
void cmd_info(int argc, char **argv);
void cmd_owns(int argc, char **argv);
void cmd_files(int argc, char **argv);
void buildmeta_collect(const char *pkgname, const char *pkgver,
                       const char *pkgrel, int is_binary,
                       const char *pkgdir, BuildMeta *m);
int  dryrun_build(char **pkgnames, int npkgs, DryRun *dr);
int  dryrun_remove(char **pkgnames, int npkgs, DryRun *dr);

void cmd_list(int argc, char **argv);
void cmd_orphans(int argc, char **argv);
int db_count_orphans(void);               /* count orphans, no output — for `lpm audit` */
int db_count_pending_updates(void);       /* count pending updates from cached repo.db, -1 if not synced */
void cmd_audit(int argc, char **argv);    /* lpm audit [--log] */

/* ── cache.c ─────────────────────────────────────────────────────────── */
void cmd_rcc(int argc, char **argv);

/* ── key.c (GPG keyring — kept from lpm.git) ─────────────────────────── */
void cmd_key(int argc, char **argv);

/* ── lpkg.c — binary package format (.lpkg) ─────────────────────────── *
 *                                                                         *
 *  lpm package build   <pkg>             build then pack → <pkg>-<ver>-<rel>-amd64.lpkg
 *  lpm package install <pkg.lpkg|name>   install from .lpkg (path or bare name scan)
 *  lpm package query   [pkg.lpkg|name]   list cached .lpkg or show package info
 *  lpm package extract <pkg.lpkg|name>   extract .lpkg into cwd for inspection
 *  lpm package verify  <pkg.lpkg|name>   verify .lpkg sha256 + meta integrity
 *  lpm package remove  <pkg.lpkg|name>   remove cached .lpkg file (not uninstall)
 * ─────────────────────────────────────────────────────────────────────── */
void cmd_pack(int argc, char **argv);            /* package pack (pkgdir must already exist) */
void cmd_build(int argc, char **argv);           /* package build (fetch+build+pack, no install) */
int  lpkg_install_from_file(const char *lpkg_path); /* internal binary install */
void cmd_pkginstall(int argc, char **argv);      /* package install */
void cmd_pkglist(int argc, char **argv);         /* package query */
void cmd_pkginstall_dir(int argc, char **argv);  /* package extract */
void cmd_pkgverify(int argc, char **argv);       /* package verify */
void cmd_pkgremove_file(int argc, char **argv);  /* package remove */

/* ── recommend.c ─────────────────────────────────────────────────────── */
int lpm_prompt_recommends(const char *pkgname, const LpmFlags *flags,
                          int (*install_fn)(const char *pkg));

/* ── sync.c — lpm upgrade ──────────────────────────────────────────────── */
/* (cmd_suy declared above in build.c section) */

/* ── build.c extras ──────────────────────────────────────────────────── */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs,
                    int maxpkgs);
void pkg_log_path(const char *pkgname, char *out, size_t outsz);
void format_size(long bytes, char *out, size_t outsz);
void check_remove_journal(void);

/* ── config.c extras ─────────────────────────────────────────────────── */
int lpm_config_is_critical(const LpmConfig *cfg, const char *pkgname);
int lpm_config_is_ignored(const LpmConfig *cfg, const char *pkgname);

#endif /* LPM_H */
