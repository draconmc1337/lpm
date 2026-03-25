#ifndef LPM_H
#define LPM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>

/* ── paths ──────────────────────────────────────────────────────────────── */
#define LPM_VERSION      "v1.1.2-alpha-hotfix1"
#define LPM_PKGBUILD_DIR "/usr/src/lpm"
#define LPM_BUILD_DIR    "/var/cache/lpm"
#define LPM_DB           "/var/lib/lpm/installed"
#define LPM_LOG          "/var/log/lpm.log"

/* ── colors ─────────────────────────────────────────────────────────────── */
#define C_RED    "\033[0;31m"
#define C_GREEN  "\033[0;32m"
#define C_YELLOW "\033[1;33m"
#define C_CYAN   "\033[0;36m"
#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

/* ── PKGBUILD struct ─────────────────────────────────────────────────────── */
#define MAX_DEPS   64
#define MAX_SRCS    4
#define MAX_STR   512

typedef struct {
    char pkgname[MAX_STR];
    char pkgver[MAX_STR];
    char pkgrel[MAX_STR];
    char depends[MAX_DEPS][MAX_STR];
    int  ndepends;
    char recommends[MAX_DEPS][MAX_STR];
    int  nrecommends;
    char makedepends[MAX_DEPS][MAX_STR];
    int  nmakedepends;
    char source[MAX_SRCS][MAX_STR];
    int  nsources;
    char sha256sums[MAX_SRCS][MAX_STR];
    char md5sums[MAX_SRCS][MAX_STR];
    char pbfile[MAX_STR];   /* full path */
    int  has_check;
    int  has_uninstall;
} Pkg;

/* ── util.c ──────────────────────────────────────────────────────────────── */
void  die(const char *fmt, ...);
void  warn(const char *fmt, ...);
void  lpm_log(const char *fmt, ...);
int   confirm(const char *prompt);
void  init_dirs(void);
void  check_root(void);
int   version_gte(const char *have, const char *need);

/* ── db.c ────────────────────────────────────────────────────────────────── */
int   db_is_installed(const char *pkgname);
char *db_get_version(const char *pkgname);   /* returns malloc'd string or NULL */
void  db_add(const char *pkgname, const char *ver, const char *rel);
void  db_remove(const char *pkgname);

/* ── pkgbuild.c ──────────────────────────────────────────────────────────── */
int   pkgbuild_parse(const char *pbfile, Pkg *pkg);
int   dep_satisfied(const char *spec);        /* "name" or "name>=ver" */

/* ── build.c ─────────────────────────────────────────────────────────────── */
void  cmd_check(int argc, char **argv);
void  cmd_remove(int argc, char **argv);
void  cmd_update(int argc, char **argv);
void  cmd_local(int argc, char **argv);
void  cmd_sync(int argc, char **argv);
void  cmd_fetch(int argc, char **argv);

/* ── search.c ────────────────────────────────────────────────────────────── */
void  cmd_search(int argc, char **argv);
void  cmd_info(int argc, char **argv);
void  cmd_list(void);

/* ── dep.c ──────────────────────────────────────────────────────────────── */
int   dep_resolve_queue(const char *pkgname, char out[][512], int maxout);
void  dep_set_folder(const char *pkgname, const char *folder);
void  cmd_deptree(int argc, char **argv);

/* ── cache.c ─────────────────────────────────────────────────────────────── */
void  cmd_rcc(int argc, char **argv);

/* ── reverse dep ─────────────────────────────────────────────────────────── */
/* returns malloc'd space-separated string of packages that depend on target */
char *reverse_deps(const char *target);

#endif /* LPM_H */
