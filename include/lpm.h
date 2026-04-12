#ifndef LPM_H
#define LPM_H

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── paths ──────────────────────────────────────────────────────────────── */
#define LPM_VERSION "v1.1.4.1-alpha"
#define LPM_PKGBUILD_DIR "/usr/src/lpm"
#define LPM_BUILD_DIR "/var/cache/lpm"
#define LPM_DB "/var/lib/lpm/installed"
#define LPM_FILES_DIR                                                          \
  "/var/lib/lpm/files"             /* per-package file ownership lists */
#define LPM_LOG_DIR "/var/log/lpm" /* per-package build logs */
#define LPM_AUDIT_LOG "/var/log/lpm/audit.log"
#define LPM_LOG "/var/log/lpm/lpm.log" /* global event log */

/* ── colors ─────────────────────────────────────────────────────────────── */
#define C_RED "\033[0;31m"
#define C_GREEN "\033[0;32m"
#define C_YELLOW "\033[1;33m"
#define C_CYAN "\033[0;36m"
#define C_BOLD "\033[1m"
#define C_RESET "\033[0m"

/* ── build/install flags ─────────────────────────────────────────────────── */
typedef struct {
  int yes;        /* --yes: skip all confirmation prompts                  */
  int strict;     /* --strict: treat check() failure as hard install block */
  int force;      /* --force: override dep/critical protection             */
  int no_confirm; /* --no-confirm: non-interactive remove (scripts)        */
} LpmFlags;

/* parse LpmFlags out of argv; remaining package names written to pkgs[].
 * Returns number of package names found. */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs,
                    int maxpkgs);
#define MAX_DEPS 64
#define MAX_SRCS 4
#define MAX_STR 512
#define MAX_CMD 2048 /* shell command buffer — must fit path + flags */

typedef struct {
  char pkgname[MAX_STR];
  char pkgver[MAX_STR];
  char pkgrel[MAX_STR];
  char depends[MAX_DEPS][MAX_STR];
  int ndepends;
  char recommends[MAX_DEPS][MAX_STR];
  int nrecommends;
  char makedepends[MAX_DEPS][MAX_STR];
  int nmakedepends;
  char source[MAX_SRCS][MAX_STR];
  int nsources;
  char sha256sums[MAX_SRCS][MAX_STR];
  char md5sums[MAX_SRCS][MAX_STR];
  char pbfile[MAX_STR]; /* full path */
  int has_check;
  int has_uninstall; /* parsed for info only — NEVER executed (security policy)
                      */
} Pkg;

/* ── util.c ──────────────────────────────────────────────────────────────── */
void die(const char *fmt, ...);
void warn(const char *fmt, ...);
void lpm_log(const char *fmt, ...);
void lpm_audit(const char *fmt,
               ...); /* security-sensitive actions → LPM_AUDIT_LOG */
int confirm(const char *prompt);
int confirm_word(const char *prompt,
                 const char *word); /* require typing exact word */
void init_dirs(void);
void check_root(void);
int version_gte(const char *have, const char *need);
/* returns per-package log path: LPM_LOG_DIR/<pkgname>.log */
void pkg_log_path(const char *pkgname, char *out, size_t outsz);

/* ── db.c ────────────────────────────────────────────────────────────────── */
int db_is_installed(const char *pkgname);
char *db_get_version(const char *pkgname); /* returns malloc'd string or NULL */
void db_add(const char *pkgname, const char *ver, const char *rel);
void db_remove(const char *pkgname);

/*
 * File ownership database — stored at LPM_FILES_DIR/<pkgname>/files.list
 * One absolute path per line. LPM owns removal; PKGBUILD never touches rootfs.
 */
void db_files_save(const char *pkgname, const char *pkgdir);
int db_files_remove(
    const char *pkgname); /* unlinks owned files, returns count */

/* ── pkgbuild.c ──────────────────────────────────────────────────────────── */
int pkgbuild_parse(const char *pbfile, Pkg *pkg);
int dep_satisfied(const char *spec); /* "name" or "name>=ver" */

/* ── build.c ─────────────────────────────────────────────────────────────── */
void cmd_sync(int argc, char **argv);
void cmd_local(int argc, char **argv);
void cmd_fetch(int argc, char **argv);
void cmd_check(int argc, char **argv);
void cmd_remove(int argc, char **argv);
void cmd_update(int argc, char **argv);

/* ── dep.c ───────────────────────────────────────────────────────────────── */
void cmd_deptree(int argc, char **argv);
int dep_resolve_queue(const char *pkgname, char out[][MAX_STR], int maxout);
void dep_set_folder(const char *pkgname, const char *folder);

/* ── search.c ────────────────────────────────────────────────────────────── */
void cmd_search(int argc, char **argv);
void cmd_info(int argc, char **argv);
void cmd_list(int argc, char **argv);

/* ── cache.c ─────────────────────────────────────────────────────────────── */
void cmd_rcc(int argc, char **argv);

/* ── build_dev.c (lpm-dev only) ──────────────────────────────────────────── */
void cmd_local_dev(int argc, char **argv);
/* returns malloc'd space-separated string of packages that depend on target */
char *reverse_deps(const char *target);

#endif /* LPM_H */
