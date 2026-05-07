#include "lpm.h"
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* ══════════════════════════════════════════════════════════════════════
 * build.c — Build, sync, remove, and update commands for lpm
 *
 * Commands implemented here:
 *   lpm -S  <pkg>   fetch PKGBUILD + resolve deps + build + install
 *   lpm -Si <pkg>   same as -S but uses local PKGBUILD (no fetch)
 *   lpm -Sc <pkg>   run check() test suite for an already-built package
 *   lpm -R  <pkg>   remove an installed package
 *   lpm -U  [pkg]   check for updates and rebuild if needed
 *   lpm -Sy <pkg>   fetch PKGBUILD only (alias: cmd_fetch)
 * ══════════════════════════════════════════════════════════════════════ */

/* g_cancel is set by the SIGINT handler in main.c */
extern volatile sig_atomic_t g_cancel;

/* Cancel-aware wrapper around system(). Returns -1 if g_cancel is set,
 * otherwise returns the child exit code via WEXITSTATUS. */
static int run(const char *cmd) {
  if (g_cancel)
    return -1;
  int rc = system(cmd);
  return (rc == -1) ? -1 : WEXITSTATUS(rc);
}

/* Macro: jump to label if g_cancel is set, printing an interruption notice. */
#define CHECK_CANCEL(label)                                                    \
  do {                                                                         \
    if (g_cancel) {                                                            \
      fprintf(stderr, "\n" C_YELLOW "warning:" C_RESET                        \
              " Interrupt received — operation aborted.\n");                   \
      goto label;                                                              \
    }                                                                          \
  } while (0)

/* Forward declaration — defined at the bottom of this file so it can
 * reference queue[] which is built by the callers above it. */
static int fetch_all_sources(char queue[][MAX_STR], int nqueue);

/* ── pkg_log_path ────────────────────────────────────────────────────── *
 * Returns the per-package build log path into out[outsz].               */
void pkg_log_path(const char *pkgname, char *out, size_t outsz) {
  snprintf(out, outsz, "%s/%s.log", LPM_LOG_DIR, pkgname);
}

/* ══════════════════════════════════════════════════════════════════════
 * lpm_parse_flags — parse CLI flags from argv into LpmFlags
 *
 * Recognised flags (all long-form):
 *   --force          override dep/conflict/critical-package checks
 *   --no-confirm     skip all yes/no prompts
 *   --strict         treat check() failure as fatal error
 *   --no-check       skip the check() phase entirely
 *   --no-recommended suppress the "install recommended?" prompt
 *
 * Non-flag arguments are collected into pkgs[] up to maxpkgs entries.
 * Returns the number of package names found.
 * ══════════════════════════════════════════════════════════════════════ */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs,
                    int maxpkgs) {
  memset(f, 0, sizeof(*f));

  /* honour LPM_DEBUG env var as baseline (CLI --debug=N overrides) */
  const char *env_dbg = getenv("LPM_DEBUG");
  if (env_dbg) {
    int lvl = atoi(env_dbg);
    if (lvl >= 1 && lvl <= 3) { f->debug = lvl; g_debug = lvl; }
  }

  int n = 0;
  for (int i = 0; i < argc; i++) {
    if      (!strcmp(argv[i], "--force"))          f->force          = 1;
    else if (!strcmp(argv[i], "--no-confirm"))     f->no_confirm     = 1;
    else if (!strcmp(argv[i], "--strict"))         f->strict         = 1;
    else if (!strcmp(argv[i], "--no-check"))       f->no_check       = 1;
    else if (!strcmp(argv[i], "--no-recommended")) f->no_recommended = 1;
    else if (!strncmp(argv[i], "--debug=", 8)) {
      int lvl = atoi(argv[i] + 8);
      if (lvl < 1) lvl = 1;
      if (lvl > 3) lvl = 3;
      f->debug = lvl;
      g_debug  = lvl;   /* set global so DBG() works everywhere */
    }
    else if (!strcmp(argv[i], "--debug")) {
      /* bare --debug without =N defaults to level 1 */
      f->debug = 1; g_debug = 1;
    }
    else if (n < maxpkgs) pkgs[n++] = argv[i];
  }

  if (g_debug >= 1)
    DBG(1, "debug mode level %d active", g_debug);

  return n;
}

/* ══════════════════════════════════════════════════════════════════════
 * fetch_url — download a single URL to dest, with retry logic
 *
 * Tries wget first, falls back to curl. On failure performs a DNS check
 * to distinguish "host unreachable" from "file not found".
 * Writes to dest.part while downloading; atomically renames on success.
 * ══════════════════════════════════════════════════════════════════════ */
#define FETCH_RETRIES 3
#define FETCH_DELAY   1

static int fetch_url(const char *url, const char *dest) {
  DBG(2, "fetch_url: %s", url);
  char part[MAX_STR];
  snprintf(part, sizeof(part), "%s.part", dest);

  /* extract hostname for DNS fallback check */
  char host[256] = "";
  const char *p = strstr(url, "://");
  if (p) {
    p += 3;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len < sizeof(host)) {
      strncpy(host, p, len);
      host[len] = '\0';
    }
  }

  for (int attempt = 1; attempt <= FETCH_RETRIES; attempt++) {
    if (g_cancel) { remove(part); return -1; }
    remove(part);

    if (attempt > 1) {
      DBG(1, "retry %d/%d for %s", attempt, FETCH_RETRIES, url);
      fprintf(stderr, C_YELLOW "  retry %d/%d" C_RESET " — waiting %ds...\n",
              attempt, FETCH_RETRIES, FETCH_DELAY);
      sleep(FETCH_DELAY);
    }

    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
             "wget -q --show-progress --timeout=30 --tries=1 -O '%s' '%s'"
             " || curl -L --progress-bar --connect-timeout 30 --max-time 120"
             " --retry 0 -o '%s' '%s'",
             part, url, part, url);
    int rc = run(cmd);

    struct stat part_st;
    int have_part = (stat(part, &part_st) == 0);

    if (rc == 0 && have_part && part_st.st_size >= 32) {
      if (rename(part, dest) == 0) {
        lpm_log("FETCH OK: %s (attempt %d)", url, attempt);
        return 0;
      }
      remove(part);
      return -1;
    }

    /* DNS check: if hostname is unresolvable, bail early instead of retrying */
    if (host[0]) {
      char dns[MAX_STR];
      snprintf(dns, sizeof(dns), "getent hosts '%s' >/dev/null 2>&1", host);
      if (run(dns) != 0) {
        fprintf(stderr, C_RED "error: " C_RESET
                "network unavailable (DNS failed for %s)\n", host);
        remove(part);
        return -1;
      }
    }

    if (have_part && part_st.st_size > 0 && part_st.st_size < 32)
      fprintf(stderr, C_YELLOW "warning: " C_RESET
              "attempt %d: response too small (%ld bytes) — likely 404\n",
              attempt, (long)part_st.st_size);
    else
      fprintf(stderr, C_YELLOW "warning: " C_RESET
              "attempt %d failed (rc=%d)\n", attempt, rc);
    remove(part);
  }

  fprintf(stderr, C_RED "error: " C_RESET
          "network timeout: %s\n  Failed after %d attempts.\n",
          url, FETCH_RETRIES);
  return -1;
}

/* ── verify_sources ──────────────────────────────────────────────────── *
 * Validates checksums for all sources declared in pkg against files     *
 * already downloaded into workspace directory ws.                       *
 * Supports sha256sums and md5sums; skips entries marked "SKIP".        *
 * Returns 1 if all checksums match, 0 on any mismatch.                 */
static int verify_sources(Pkg *pkg, const char *ws) {
  int ok = 1;
  for (int i = 0; i < pkg->nsources; i++) {
    const char *expected = NULL, *algo = NULL, *tool = NULL;

    if (pkg->sha256sums[i][0] && strcmp(pkg->sha256sums[i], "SKIP") != 0) {
      expected = pkg->sha256sums[i]; algo = "sha256"; tool = "sha256sum";
    } else if (pkg->md5sums[i][0] && strcmp(pkg->md5sums[i], "SKIP") != 0) {
      expected = pkg->md5sums[i]; algo = "md5"; tool = "md5sum";
    } else
      continue;

    char *fname = strrchr(pkg->source[i], '/');
    if (!fname) continue;
    fname++;

    char filepath[MAX_STR];
    snprintf(filepath, sizeof(filepath), "%s/%s", ws, fname);

    char cmd[MAX_STR];
    snprintf(cmd, sizeof(cmd), "%s '%s' 2>/dev/null | cut -d' ' -f1",
             tool, filepath);
    FILE *fp = popen(cmd, "r");
    if (!fp) { ok = 0; continue; }

    char actual[MAX_STR] = "";
    if (fgets(actual, sizeof(actual), fp))
      actual[strcspn(actual, "\n")] = '\0';
    pclose(fp);

    if (strcmp(actual, expected) != 0) {
      fprintf(stderr,
              C_RED "error: " C_RESET "%s mismatch for " C_BOLD "%s" C_RESET
              "\n  expected: " C_CYAN "%s" C_RESET
              "\n  got:      " C_RED  "%s" C_RESET "\n",
              algo, fname, expected, actual);
      ok = 0;
    } else {
      printf(C_GREEN "  ok" C_RESET " [%s] %s\n", algo, fname);
    }
  }
  return ok;
}

/* ══════════════════════════════════════════════════════════════════════
 * Remove journal — crash-safe tracking of in-progress removes
 *
 * File: /var/lib/lpm/db/remove_journal
 * Format: one package name per line, appended before remove starts,
 *         removed after each package completes cleanly.
 *
 * On the next lpm invocation, check_remove_journal() warns the user if
 * the journal is non-empty (meaning a previous remove was interrupted).
 * ══════════════════════════════════════════════════════════════════════ */
#define REMOVE_JOURNAL "/var/lib/lpm/db/remove_journal"

/* Append pkgname to the journal before starting its removal. */
static void journal_begin(const char *pkgname) {
  FILE *f = fopen(REMOVE_JOURNAL, "a");
  if (f) { fprintf(f, "%s\n", pkgname); fclose(f); }
}

/* Remove pkgname from the journal after its removal completes.
 * Deletes the file entirely if it becomes empty. */
static void journal_done(const char *pkgname) {
  FILE *f = fopen(REMOVE_JOURNAL, "r");
  if (!f) return;

  char tmp[LPM_PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.tmp", REMOVE_JOURNAL);
  FILE *t = fopen(tmp, "w");
  if (!t) { fclose(f); return; }

  char line[LPM_NAME_MAX];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    if (strcmp(line, pkgname) != 0)
      fprintf(t, "%s\n", line);
  }
  fclose(f);
  fclose(t);
  rename(tmp, REMOVE_JOURNAL);

  /* delete journal if now empty */
  f = fopen(REMOVE_JOURNAL, "r");
  if (f) {
    int empty = 1;
    char ch;
    while ((ch = fgetc(f)) != EOF)
      if (!isspace(ch)) { empty = 0; break; }
    fclose(f);
    if (empty) unlink(REMOVE_JOURNAL);
  }
}

/* Print a warning if the remove journal is non-empty (interrupted remove). */
void check_remove_journal(void) {
  struct stat st;
  if (stat(REMOVE_JOURNAL, &st) != 0) return;

  FILE *f = fopen(REMOVE_JOURNAL, "r");
  if (!f) return;

  char line[LPM_NAME_MAX];
  int n = 0;
  fprintf(stderr, C_YELLOW "warning: " C_RESET
          "Previous remove was interrupted. Affected packages:\n");
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0]) { fprintf(stderr, "  %s\n", line); n++; }
  }
  fclose(f);
  if (n > 0)
    fprintf(stderr, "  Run 'lpm -Qk' to check integrity.\n");
}

/* ── install_one — callback for lpm_prompt_recommends ───────────────── */
static int install_one(const char *pkgname) {
  char *pair[1] = { (char *)pkgname };
  cmd_sync(1, pair);
  return db_is_installed(pkgname) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════════════════
 * do_build_install — shared build + install core
 *
 * Given a parsed Pkg and its PKGBUILD path, this function:
 *   1. Extracts downloaded sources into the workspace
 *   2. Runs build() inside bash
 *   3. Optionally runs check() (respects --no-check / --strict)
 *   4. Runs package() to stage files into pkgdir
 *   5. Optionally strips binaries/libraries
 *   6. Commits the staged files via the transaction layer
 *
 * qi/nqueue are used only for the "[N/M] Building …" progress line.
 * ══════════════════════════════════════════════════════════════════════ */

/* ── pkgbuild_to_bash ────────────────────────────────────────────────── *
 * Convert new-format PKGBUILD (key = value / build { }) into a          *
 * bash-sourceable temp file. Returns path to temp file (caller frees),  *
 * or NULL if old format (no conversion needed).                         *
 *                                                                        *
 * Conversion rules:                                                      *
 *   key = value          →  key=value                                    *
 *   key = ("a" ; "b")   →  key=("a" "b")                                *
 *   build {              →  build() {                                    *
 *   package {            →  package() {                                  *
 * ─────────────────────────────────────────────────────────────────────── */
static char *pkgbuild_to_bash(const char *pbfile) {
    /* detect new format */
    FILE *fin = fopen(pbfile, "r");
    if (!fin) return NULL;
    char line[4096];
    int is_new = 0;
    while (fgets(line, sizeof(line), fin)) {
        if (strstr(line, " = ") ||
            strncmp(line, "pkgtype", 7) == 0) { is_new = 1; break; }
    }
    fclose(fin);
    if (!is_new) return NULL;   /* old format — no conversion needed */

    /* write converted temp file */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/lpm_pkgbuild_%d.sh", (int)getpid());

    fin  = fopen(pbfile,  "r");
    FILE *fout = fopen(tmppath, "w");
    if (!fin || !fout) { if (fin) fclose(fin); if (fout) fclose(fout); return NULL; }

    /* helper: strip leading/trailing whitespace in-place */
    while (fgets(line, sizeof(line), fin)) {
        char *p = line;
        /* skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        /* strip trailing newline */
        char *end = p + strlen(p) - 1;
        while (end >= p && (*end == '\n' || *end == '\r')) *end-- = '\0';

        /* blank / comment lines */
        if (!*p || *p == '#') { fprintf(fout, "%s\n", p); continue; }

        /* function: "build {" / "package {" / "check {" / "remove {" */
        const char *funcs[] = {"build","package","check","remove",NULL};
        int is_func = 0;
        for (int fi = 0; funcs[fi]; fi++) {
            size_t fl = strlen(funcs[fi]);
            if (strncmp(p, funcs[fi], fl) == 0) {
                char *after = p + fl;
                /* skip optional "()" already there */
                if (after[0] == '(' && after[1] == ')') after += 2;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == '{' || *after == '\0') {
                    /* emit as "funcname() {" */
                    fprintf(fout, "%s() {%s\n", funcs[fi],
                            (*after == '{') ? after + 1 : "");
                    is_func = 1; break;
                }
            }
        }
        if (is_func) continue;

        /* closing brace */
        if (strcmp(p, "}") == 0) { fprintf(fout, "}\n"); continue; }

        /* key = value line */
        char *eq = strchr(p, '=');
        if (!eq) { fprintf(fout, "%s\n", p); continue; }

        /* check space before = (new format indicator) */
        if (eq > p && *(eq-1) == ' ') {
            char key[64] = "";
            char *ke = eq - 1;
            while (ke > p && (*ke == ' ' || *ke == '\t')) ke--;
            int kl = (int)(ke - p + 1);
            if (kl > 0 && kl < 63) { strncpy(key, p, kl); key[kl] = '\0'; }

            char *val = eq + 1;
            while (*val == ' ') val++;

            /* skip pkgtype — not valid bash */
            if (strcmp(key, "pkgtype") == 0) continue;

            if (*val == '(') {
                /* array: convert semicolons to spaces, remove quotes optionally */
                char converted[4096] = "(";
                /* collect until closing ) possibly on next lines */
                char full[4096];
                strncpy(full, val, sizeof(full)-1);
                /* if no closing paren yet, read more lines */
                while (!strchr(full, ')')) {
                    char more[512];
                    if (!fgets(more, sizeof(more), fin)) break;
                    strncat(full, more, sizeof(full)-strlen(full)-1);
                }
                char *open  = strchr(full, '(');
                char *close = strrchr(full, ')');
                if (open && close && close > open) {
                    *close = '\0';
                    char *c = open + 1;
                    int first = 1;
                    strcpy(converted, "(");
                    while (*c) {
                        while (*c == ' ' || *c == '\t' || *c == '\n') c++;
                        if (!*c) break;
                        if (*c == ';') { c++; continue; }
                        /* read element */
                        char elem[256] = "";
                        int ei = 0;
                        if (*c == '"') {
                            c++;
                            while (*c && *c != '"' && ei < 255) elem[ei++] = *c++;
                            if (*c == '"') c++;
                        } else {
                            while (*c && *c != ' ' && *c != ';' && *c != ')' && ei < 255)
                                elem[ei++] = *c++;
                        }
                        elem[ei] = '\0';
                        if (elem[0]) {
                            if (!first) strncat(converted, " ", sizeof(converted)-strlen(converted)-1);
                            strncat(converted, "\"", sizeof(converted)-strlen(converted)-1);
                            strncat(converted, elem, sizeof(converted)-strlen(converted)-1);
                            strncat(converted, "\"", sizeof(converted)-strlen(converted)-1);
                            first = 0;
                        }
                    }
                    strncat(converted, ")", sizeof(converted)-strlen(converted)-1);
                }
                fprintf(fout, "%s=%s\n", key, converted);
            } else {
                /* scalar: just remove the space around = */
                fprintf(fout, "%s=%s\n", key, val);
            }
        } else {
            /* old-style line inside new format file — pass through */
            fprintf(fout, "%s\n", p);
        }
    }

    fclose(fin);
    fclose(fout);

    char *result = strdup(tmppath);
    return result;
}

static void do_build_install(Pkg *pkg, const char *pbfile_orig, LpmConfig *cfg,
                             int qi, int nqueue, const LpmFlags *flags) {
  /* Convert new-format PKGBUILD to bash-compatible temp file if needed */
  char *_converted_pb = pkgbuild_to_bash(pbfile_orig);
  const char *pbfile  = _converted_pb ? _converted_pb : pbfile_orig;
#define PBCLEAN() do { if (_converted_pb) { unlink(_converted_pb); free(_converted_pb); _converted_pb = NULL; } } while(0)

  if (g_cancel) { PBCLEAN(); return; }

  char ws[MAX_STR];
  snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg->pkgname);
  mkdir(ws, 0755);

  /* extract each source archive into the workspace */
  for (int i = 0; i < pkg->nsources; i++) {
    if (!pkg->source[i][0]) continue;
    char *fname = strrchr(pkg->source[i], '/');
    if (!fname) continue;
    fname++;

    char srcpath[MAX_STR];
    snprintf(srcpath, sizeof(srcpath), "%s/%s", ws, fname);

    struct stat sst;
    if (stat(srcpath, &sst) != 0) continue; /* not downloaded yet */

    char excmd[MAX_CMD];
    if      (strstr(fname, ".tar.gz")  || strstr(fname, ".tgz"))
      snprintf(excmd, sizeof(excmd),
               "tar -xzf '%s' -C '%s' 2>/dev/null || true", srcpath, ws);
    else if (strstr(fname, ".tar.xz"))
      snprintf(excmd, sizeof(excmd),
               "tar -xJf '%s' -C '%s' 2>/dev/null || true", srcpath, ws);
    else if (strstr(fname, ".tar.bz2"))
      snprintf(excmd, sizeof(excmd),
               "tar -xjf '%s' -C '%s' 2>/dev/null || true", srcpath, ws);
    else if (strstr(fname, ".tar.zst"))
      snprintf(excmd, sizeof(excmd),
               "tar --zstd -xf '%s' -C '%s' 2>/dev/null || true", srcpath, ws);
    else if (strstr(fname, ".zip"))
      snprintf(excmd, sizeof(excmd),
               "unzip -q '%s' -d '%s' 2>/dev/null || true", srcpath, ws);
    else if (strstr(fname, ".patch") || strstr(fname, ".diff"))
      continue; /* patches stay in ws root, applied by build() */
    else
      continue;

    if (g_verbose)
      printf(C_GRAY "  -> extracting %s\n" C_RESET, fname);
    run(excmd);
  }

  char pkg_log[MAX_STR];
  pkg_log_path(pkg->pkgname, pkg_log, sizeof(pkg_log));

  time_t t_start = time(NULL);

  printf(C_BOLD "[%d/%d] Building %s %s-%s" C_RESET "\n",
         qi + 1, nqueue, pkg->pkgname, pkg->pkgver, pkg->pkgrel);
  lpm_log("Building %s %s-%s", pkg->pkgname, pkg->pkgver, pkg->pkgrel);

  /* run build() function from PKGBUILD */
  char build_cmd[MAX_CMD];
  snprintf(build_cmd, sizeof(build_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " SOURCE_DATE_EPOCH=\"%ld\""
           " && build' > \"%s\" 2>&1",
           pbfile, ws,
           cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx,
           (long)t_start, pkg_log);

  if (run(build_cmd) != 0) {
    if (g_cancel) return;
    fprintf(stderr, C_RED "error: " C_RESET
            "Build failed: %s\n  Phase: build()\n  Log: " C_CYAN "%s" C_RESET "\n",
            pkg->pkgname, pkg_log);
    lpm_log("Build FAILED: %s", pkg->pkgname);
    PBCLEAN(); exit(1);
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* optional check() phase */
  if (pkg->has_check && !flags->no_check) {
    printf(C_CYAN "::" C_RESET " Running check()...\n");
    char check_cmd[MAX_CMD];
    snprintf(check_cmd, sizeof(check_cmd),
             "bash -c 'source \"%s\" && cd \"%s\" && check' >> \"%s\" 2>&1",
             pbfile, ws, pkg_log);
    int rc = run(check_cmd);

    /* always show the last 40 lines of the log after check */
    char tail[MAX_STR];
    snprintf(tail, sizeof(tail), "tail -40 '%s'", pkg_log);
    run(tail);

    if (rc != 0) {
      if (flags->strict) {
        fprintf(stderr, C_RED "error: " C_RESET
                "check() failed (rc=%d) — --strict\n  Log: " C_CYAN "%s" C_RESET "\n",
                rc, pkg_log);
        PBCLEAN(); exit(1);
      }
      printf(C_YELLOW "warning: " C_RESET
             "check() failed (rc=%d)\n  Log: " C_CYAN "%s" C_RESET "\n",
             rc, pkg_log);
      if (!flags->no_confirm)
        if (!confirm(C_CYAN "::" C_RESET " Dependency issue detected. Continue anyway? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] "))
          { PBCLEAN(); exit(1); }
    } else {
      printf(C_GREEN "  check() passed" C_RESET "\n");
    }
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* run package() to populate pkgdir (the staging directory) */
  char pkgdir[MAX_STR];
  snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", ws);
  char mk_cmd[MAX_STR];
  snprintf(mk_cmd, sizeof(mk_cmd), "rm -rf '%s' && mkdir -p '%s'", pkgdir, pkgdir);
  run(mk_cmd);

  printf(C_BOLD "==> Staging %s" C_RESET "\n", pkg->pkgname);
  lpm_log("Staging %s", pkg->pkgname);

  char inst_cmd[MAX_CMD];
  snprintf(inst_cmd, sizeof(inst_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " pkgdir=\"%s\" SOURCE_DATE_EPOCH=\"%ld\""
           " && package' >> \"%s\" 2>&1",
           pbfile, ws,
           cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx,
           pkgdir, (long)t_start, pkg_log);

  if (run(inst_cmd) != 0) {
    if (g_cancel) return;
    fprintf(stderr, C_RED "error: " C_RESET
            "Install failed for %s\n  Phase: package()\n  Log: " C_CYAN "%s" C_RESET "\n",
            pkg->pkgname, pkg_log);
    lpm_log("Install FAILED: %s", pkg->pkgname);
    PBCLEAN(); exit(1);
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* strip debug symbols from shared libs and executables unless keep_pkg is set */
  if (cfg->keep_pkg == 0) {
    char strip_cmd[MAX_STR];
    snprintf(strip_cmd, sizeof(strip_cmd),
             "find '%s' -type f \\( -name '*.so*' -o -perm /111 \\)"
             " -exec strip --strip-unneeded '{}' + 2>/dev/null || true",
             pkgdir);
    run(strip_cmd);
  }

  /* build a Package struct for the transaction layer */
  Package mpkg;
  memset(&mpkg, 0, sizeof(mpkg));
  strncpy(mpkg.name,    pkg->pkgname, LPM_NAME_MAX - 1);
  strncpy(mpkg.version, pkg->pkgver,  LPM_VER_MAX  - 1);
  strncpy(mpkg.release, pkg->pkgrel,  15);
  strncpy(mpkg.pkg_dir, pkgdir,       LPM_PATH_MAX - 1);
  mpkg.type   = PKG_TYPE_SOURCE;
  mpkg.reason = REASON_EXPLICIT;
  mpkg.state  = PKG_STATE_STAGED;

  Transaction *tx = tx_new();
  if (!tx) {
    fprintf(stderr, C_RED "error: " C_RESET
            "out of memory allocating transaction for %s\n", pkg->pkgname);
    PBCLEAN(); exit(1);
  }

  if (tx_add_install(tx, &mpkg) != 0) { PBCLEAN(); tx_free(tx); exit(1); }

  Package *mpkg_arr[1] = { &mpkg };
  if (safety_check_conflicts(mpkg_arr, 1, "/") != 0 && !flags->force) {
    fprintf(stderr, C_RED "error: " C_RESET
            "named conflicts block install of %s\n", pkg->pkgname);
    PBCLEAN(); tx_free(tx);
    exit(1);
  }

  if (tx_commit(tx, "/") != 0) {
    if (g_cancel) { PBCLEAN(); tx_free(tx); return; }
    fprintf(stderr, C_RED "error: " C_RESET
            "Install failed for %s — transaction rolled back\n", pkg->pkgname);
    tx_free(tx);
    exit(1);
  }

  tx_free(tx);
  PBCLEAN();
  lpm_audit("install: %s %s-%s", pkg->pkgname, pkg->pkgver, pkg->pkgrel);

  long elapsed = (long)(time(NULL) - t_start);
  printf(C_GREEN "==> Installed %s %s-%s" C_RESET
         " (" C_GRAY "%ldm%lds" C_RESET ")\n",
         pkg->pkgname, pkg->pkgver, pkg->pkgrel,
         elapsed / 60, elapsed % 60);
  lpm_log("Installed %s %s-%s (build time: %lds)",
          pkg->pkgname, pkg->pkgver, pkg->pkgrel, elapsed);

  /* clean up extracted sources if keep_src is not set */
  if (!cfg->keep_src) {
    char src_path[MAX_STR];
    snprintf(src_path, sizeof(src_path), "%s/src", ws);
    util_rmrf(src_path);
  }
}
#undef PBCLEAN   /* scope: do_build_install only */

/* ══════════════════════════════════════════════════════════════════════
 * fetch_pkgbuild — download pkgbuild_<name> from the upstream repo
 *
 * Tries each folder (base / extra / lotus) in order and stops at the
 * first successful fetch. Invalidates both the disk metadata cache and
 * the in-process PkgMeta cache so the next dep_resolve uses fresh data.
 * ══════════════════════════════════════════════════════════════════════ */
#define REPO_BASE \
  "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"

static const char *FOLDERS[] = { "base", "extra", "lotus" };
#define NFOLDERS 3

static int fetch_pkgbuild(const char *name) {
  char dest[MAX_STR];
  snprintf(dest, sizeof(dest), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
  printf(C_CYAN "::" C_RESET " Fetching pkgbuild_%s...\n", name);

  for (int f = 0; f < NFOLDERS; f++) {
    char url[MAX_STR];
    snprintf(url, sizeof(url), "%s/%s/pkgbuild_%s", REPO_BASE, FOLDERS[f], name);

    if (fetch_url(url, dest) == 0) {
      struct stat st;
      if (stat(dest, &st) == 0 && st.st_size > 32) {
        DBG(1, "selected repo [%s] for pkgbuild_%s", FOLDERS[f], name);
        printf(C_GREEN "  ->" C_RESET " Found in " C_CYAN "[%s]" C_RESET "\n",
               FOLDERS[f]);
        lpm_log("Fetched pkgbuild_%s from %s/%s", name, REPO_BASE, FOLDERS[f]);
        pkgbuild_invalidate_cache(name); /* invalidate disk metadata cache */
        dep_meta_cache_invalidate();     /* invalidate in-process PkgMeta cache */
        return 0;
      }
    }
    remove(dest);
  }
  return -1;
}

/* ── cmd_fetch (-Sy) ─────────────────────────────────────────────────── */
void cmd_fetch(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm -Sy <package>");
  for (int i = 0; i < argc; i++) {
    if (fetch_pkgbuild(argv[i]) != 0)
      fprintf(stderr, C_RED "error: " C_RESET "pkgbuild_%s not found\n", argv[i]);
  }
  printf(C_CYAN "::" C_RESET " PKGBUILDs saved to " C_BOLD "%s" C_RESET "\n"
         "   Review then run: lpm -Si <package>\n",
         LPM_PKGBUILD_DIR);
}

/* ══════════════════════════════════════════════════════════════════════
 * build_queue — resolve and topologically sort the full build queue
 *
 * PERF: uses dep_resolve_queue_multi() to collect all packages and their
 * transitive dependencies in a single pass, then toposort once.
 *
 * Old approach (O(N×M) parse calls):
 *   for each pkg: dep_resolve_queue(pkg) — reset + collect + toposort
 *
 * New approach (O(N+M) parse calls, in-process cache hits):
 *   dep_resolve_queue_multi(pkgs, N) — collect all → toposort once
 *
 * After resolving, fetches any PKGBUILDs that are not yet on disk.
 * Returns the number of entries written into queue[].
 * ══════════════════════════════════════════════════════════════════════ */
static int build_queue(char **pkgs, int npkgs, char queue[][MAX_STR], int maxq) {
  /* collect all packages + transitive deps, toposort in one shot */
  int nqueue = dep_resolve_queue_multi(pkgs, npkgs, queue, maxq, 0);

  /* fetch any PKGBUILDs that are missing from disk */
  for (int j = 0; j < nqueue; j++) {
    char pbf[MAX_STR];
    snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[j]);
    struct stat pbst;
    if (stat(pbf, &pbst) != 0)
      fetch_pkgbuild(queue[j]);
  }
  return nqueue;
}

/* ── cmd_local (-Si) ─────────────────────────────────────────────────── *
 * Build and install using local PKGBUILDs (no network fetch).           *
 * Requires that pkgbuild_<name> already exists in LPM_PKGBUILD_DIR.    */
void cmd_local(int argc, char **argv) {
  check_root();
  init_dirs();
  check_remove_journal();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -Si <package>");

  /* verify all requested PKGBUILDs exist before doing any work */
  for (int i = 0; i < npkgs; i++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgs[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0)
      die("PKGBUILD not found for '%s'\n  Use 'lpm -S' to fetch.", pkgs[i]);
  }

  char queue[256][MAX_STR];
  int nqueue = build_queue(pkgs, npkgs, queue, 256);

  cmd_deptree(npkgs, pkgs);

  if (nqueue > 0) {
    printf(C_CYAN "::" C_RESET " Will build " C_BOLD "%d" C_RESET " package(s):\n",
           nqueue);
    for (int i = 0; i < nqueue; i++)
      printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
    printf("\n");
  }

  if (!flags.no_confirm)
    if (!confirm(C_CYAN "::" C_RESET " Proceed with build? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
      printf(C_YELLOW "Operation cancelled." C_RESET "\n");
      return;
    }

  if (fetch_all_sources(queue, nqueue) != 0)
    die("Source fetch failed.");

  for (int qi = 0; qi < nqueue; qi++) {
    CHECK_CANCEL(build_done);
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) {
      warn("No PKGBUILD for '%s', skipping", queue[qi]);
      continue;
    }
    if (db_is_installed(queue[qi])) {
      printf(C_CYAN "  ->" C_RESET " %s already installed, skipping\n", queue[qi]);
      continue;
    }
    do_build_install(&pkg, pbfile, &cfg, qi, nqueue, &flags);

    if (!g_cancel && !flags.no_recommended)
      lpm_prompt_recommends(queue[qi], &flags, install_one);
  }
build_done:;
}

/* ── cmd_sync (-S) ───────────────────────────────────────────────────── *
 * Fetch PKGBUILD + resolve deps + build + install.                      *
 * Full online sync: always fetches the PKGBUILD from the repo first.   */
void cmd_sync(int argc, char **argv) {
  check_root();
  init_dirs();
  check_remove_journal();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -S <package>");

  /* fetch PKGBUILDs for explicitly requested packages */
  for (int i = 0; i < npkgs; i++)
    if (fetch_pkgbuild(pkgs[i]) != 0)
      die("pkgbuild_%s not found in repo.", pkgs[i]);

  char queue[256][MAX_STR];
  int nqueue = build_queue(pkgs, npkgs, queue, 256);

  cmd_deptree(npkgs, pkgs);

  if (nqueue > 0) {
    printf(C_CYAN "::" C_RESET " Will build " C_BOLD "%d" C_RESET " package(s):\n",
           nqueue);
    for (int i = 0; i < nqueue; i++)
      printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
    printf("\n");
  }

  if (!flags.no_confirm)
    if (!confirm(C_CYAN "::" C_RESET " Proceed with build? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
      printf(C_YELLOW "Operation cancelled." C_RESET "\n");
      return;
    }

  /* fetch any dep PKGBUILDs that build_queue didn't already pull */
  for (int qi = 0; qi < nqueue; qi++) {
    CHECK_CANCEL(sync_done);
    if (db_is_installed(queue[qi])) continue;
    char pbf[MAX_STR];
    snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    struct stat st;
    if (stat(pbf, &st) != 0) {
      printf(C_CYAN "  ->" C_RESET " Fetching pkgbuild_%s...\n", queue[qi]);
      fetch_pkgbuild(queue[qi]);
    }
  }

  if (fetch_all_sources(queue, nqueue) != 0)
    die("Source fetch failed.");

  for (int qi = 0; qi < nqueue; qi++) {
    CHECK_CANCEL(sync_done);
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) {
      warn("No PKGBUILD for '%s', skipping", queue[qi]);
      continue;
    }
    if (db_is_installed(queue[qi])) {
      printf(C_CYAN "  ->" C_RESET " %s already installed, skipping\n", queue[qi]);
      continue;
    }
    do_build_install(&pkg, pbfile, &cfg, qi, nqueue, &flags);

    if (!g_cancel && !flags.no_recommended)
      lpm_prompt_recommends(queue[qi], &flags, install_one);
  }
sync_done:;
}

/* ── cmd_check (-Sc) ─────────────────────────────────────────────────── *
 * Run the check() test suite for a package that has already been built. *
 * Does not install anything; only runs the test phase.                  */
void cmd_check(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm -Sc <package>");

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgargs[256];
  int nargs = lpm_parse_flags(argc, argv, &flags, pkgargs, 256);
  if (nargs == 0) { pkgargs[0] = NULL; }
  (void)pkgargs;

  for (int i = 0; i < argc; i++) {
    CHECK_CANCEL(check_done);
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0)
      die("PKGBUILD not found for '%s'", argv[i]);

    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);

    if (!pkg.has_check) {
      warn("No check() in pkgbuild_%s — skipping", argv[i]);
      continue;
    }

    if (!flags.no_confirm) {
      char prompt[MAX_STR];
      snprintf(prompt, sizeof(prompt),
               "Run test suite for " C_BOLD "%s" C_RESET "? [Yes/No] ", argv[i]);
      if (!confirm(prompt)) { printf("Skipped.\n"); continue; }
    }

    char pkg_log[MAX_STR];
    pkg_log_path(pkg.pkgname, pkg_log, sizeof(pkg_log));

    printf(C_CYAN "::" C_RESET " Running check() for " C_BOLD "%s" C_RESET "...\n",
           argv[i]);
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
             "bash -c 'source \"%s\" && cd \"%s\" && check' > \"%s\" 2>&1",
             pbfile, ws, pkg_log);
    int rc = run(cmd);

    char tail[MAX_STR];
    snprintf(tail, sizeof(tail), "tail -40 '%s'", pkg_log);
    run(tail);

    if (rc == 0) {
      printf(C_GREEN "check() passed" C_RESET "\n");
      lpm_log("check() passed for %s", argv[i]);
    } else {
      printf(C_RED "check() failed (rc=%d)" C_RESET
             "  Log: " C_CYAN "%s" C_RESET "\n", rc, pkg_log);
      lpm_log("check() failed for %s (rc=%d)", argv[i], rc);
      if (!flags.no_confirm)
        if (!confirm(C_CYAN "::" C_RESET " check() failed. Continue anyway? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] "))
          { printf(C_YELLOW "Operation cancelled." C_RESET "\n"); exit(1); }
    }
  }
check_done:;
}

/* ── cmd_remove (-R) ─────────────────────────────────────────────────── *
 * Remove one or more installed packages.                                *
 *                                                                        *
 * Safety checks performed before removal:                               *
 *   - reverse dependency check (blocks removal if others depend on pkg) *
 *   - CriticalPkg protection (requires triple confirmation + "DELETE")  *
 *   - installed check (refuses to remove packages that aren't there)    *
 *                                                                        *
 * Each package removal is wrapped in the remove journal for crash safety.*
 * ─────────────────────────────────────────────────────────────────────── */
void cmd_remove(int argc, char **argv) {
  check_root();
  init_dirs();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[64];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 64);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -R <package> [--force] [--no-confirm]");

  /* audit log when elevated flags are used */
  if (flags.force || flags.no_confirm) {
    char pkglist[512] = "";
    for (int i = 0; i < npkgs; i++) {
      if (i) strncat(pkglist, " ", sizeof(pkglist) - strlen(pkglist) - 1);
      strncat(pkglist, pkgs[i], sizeof(pkglist) - strlen(pkglist) - 1);
    }
    lpm_audit("remove flags: force=%d no_confirm=%d packages: %s",
              flags.force, flags.no_confirm, pkglist);
  }

  /* reverse dependency check */
  int blocked = 0;
  for (int i = 0; i < npkgs; i++) {
    char *rdeps = reverse_deps(pkgs[i]);
    if (rdeps && rdeps[0]) {
      fprintf(stderr, C_RED "error: " C_RESET C_BOLD "%s" C_RESET
              " is required by: " C_YELLOW "%s" C_RESET "\n", pkgs[i], rdeps);
      blocked = 1;
    }
  }
  if (blocked && !flags.force) {
    printf("Remove dependents first, or use " C_BOLD "-R --force" C_RESET ".\n");
    exit(1);
  }
  if (blocked && flags.force)
    warn("--force: ignoring reverse dependencies.");

  /* installed check */
  int any_missing = 0;
  for (int i = 0; i < npkgs; i++) {
    if (!db_is_installed(pkgs[i])) {
      fprintf(stderr, C_RED "error: " C_RESET "not installed: %s\n", pkgs[i]);
      any_missing = 1;
    }
  }
  if (any_missing) exit(1);

  /* CriticalPkg protection: requires triple typed confirmation */
  char crit_upper[512] = "";
  int ncrit = 0;
  for (int i = 0; i < npkgs; i++) {
    if (!lpm_config_is_critical(&cfg, pkgs[i])) continue;
    if (!flags.force) {
      fprintf(stderr, C_RED "error: " C_RESET "'%s' is protected (CriticalPkg)\n"
              "Use " C_BOLD "--force" C_RESET " to override.\n", pkgs[i]);
      exit(1);
    }
    if (ncrit > 0)
      strncat(crit_upper, ";", sizeof(crit_upper) - strlen(crit_upper) - 1);
    char upper[64];
    snprintf(upper, sizeof(upper), "%s", pkgs[i]);
    for (char *u = upper; *u; u++)
      *u = (*u >= 'a' && *u <= 'z') ? *u - 32 : *u;
    strncat(crit_upper, upper, sizeof(crit_upper) - strlen(crit_upper) - 1);
    ncrit++;
  }
  if (ncrit > 0) {
    fprintf(stderr, C_RED C_BOLD "\nERROR: %s %s A CRITICAL PACKAGE!\n" C_RESET,
            crit_upper, ncrit > 1 ? "ARE" : "IS");
    if (!flags.no_confirm) {
      if (!confirm_word("Type " C_BOLD "YES" C_RESET " to continue:\n> ", "YES"))
        { printf(C_YELLOW "Operation cancelled." C_RESET "\n"); exit(0); }
      if (!confirm_word(C_BOLD "FINAL CONFIRMATION (type YES):\n> " C_RESET, "YES"))
        { printf(C_YELLOW "Operation cancelled." C_RESET "\n"); exit(0); }
      if (!confirm_word("Type " C_BOLD "DELETE" C_RESET " to proceed:\n> ", "DELETE"))
        { printf(C_YELLOW "Operation cancelled." C_RESET "\n"); exit(0); }
    }
  }

  if (!flags.no_confirm) {
    printf("Packages to remove (" C_BOLD "%d" C_RESET "):\n", npkgs);
    for (int i = 0; i < npkgs; i++) printf("  %s\n", pkgs[i]);
    printf("\n");
    if (!confirm(C_CYAN "::" C_RESET " Proceed with removal? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
      printf(C_YELLOW "Operation cancelled." C_RESET "\n");
      return;
    }
  }

  int interrupted = 0;
  for (int i = 0; i < npkgs; i++) {
    if (g_cancel) { interrupted = 1; break; }

    journal_begin(pkgs[i]);

    printf(C_CYAN "::" C_RESET " Removing " C_BOLD "%s" C_RESET "...", pkgs[i]);
    fflush(stdout);
    lpm_log("Removing %s", pkgs[i]);

    int nfiles = db_files_remove(pkgs[i]);
    if (g_cancel) { interrupted = 1; break; }

    if (nfiles < 0)
      warn("no files.list for '%s' — only removing DB", pkgs[i]);
    else
      printf(" (%d file(s))", nfiles);

    db_remove(pkgs[i]);

    /* clean up any leftover build workspace */
    char cache[MAX_STR], rm_cmd[MAX_STR];
    snprintf(cache,  sizeof(cache),  "%s/%s", LPM_BUILD_DIR, pkgs[i]);
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
    run(rm_cmd);

    journal_done(pkgs[i]);

    printf(" " C_GREEN "done" C_RESET "\n");
    lpm_log("Removed %s (files=%d)", pkgs[i], nfiles);
    lpm_audit("remove: %s", pkgs[i]);
  }

  if (interrupted) {
    fprintf(stderr, "\n" C_YELLOW "warning: " C_RESET
            "Remove aborted mid-flight — DB may be inconsistent.\n"
            "  Run 'lpm -Qk' to check integrity.\n");
  }
}

/* ── cmd_update (-U) ─────────────────────────────────────────────────── *
 * Check for updates against local PKGBUILDs and rebuild if needed.     *
 * With no arguments, checks every installed package.                   *
 * With arguments, checks only the specified packages.                  */
void cmd_update(int argc, char **argv) {
  check_root();
  init_dirs();
  check_remove_journal();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *flagargs[256];
  lpm_parse_flags(argc, argv, &flags, flagargs, 256);

  char *targets[256];
  int ntargets = 0;

  if (argc == 0) {
    /* no args: check all installed packages */
    FILE *f = fopen(LPM_DB, "r");
    if (!f) { printf("No packages installed via lpm.\n"); return; }
    char line[MAX_STR];
    while (fgets(line, sizeof(line), f) && ntargets < 256) {
      line[strcspn(line, "\n")] = '\0';
      if (!line[0]) continue;
      char *eq = strchr(line, '=');
      if (eq) *eq = '\0';
      targets[ntargets++] = strdup(line);
    }
    fclose(f);
    printf(C_CYAN "::" C_RESET " Checking updates for " C_BOLD "%d" C_RESET
           " package(s)...\n\n", ntargets);
  } else {
    for (int i = 0; i < argc && i < 256; i++)
      targets[ntargets++] = argv[i];
  }

  char *to_update[256];
  int nupdate = 0;

  for (int i = 0; i < ntargets; i++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, targets[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0) { warn("No PKGBUILD for '%s'", targets[i]); continue; }

    if (lpm_config_is_ignored(&cfg, targets[i])) {
      printf("  " C_BOLD "%-24s" C_RESET "  " C_CYAN "ignored" C_RESET "\n", targets[i]);
      continue;
    }

    Pkg pkg;
    pkgbuild_parse(pbfile, &pkg);
    char pb_full[MAX_STR];
    snprintf(pb_full, sizeof(pb_full), "%s-%s", pkg.pkgver, pkg.pkgrel);

    char *inst_ver = db_get_version(targets[i]);
    if (!inst_ver) {
      printf("  " C_BOLD "%-24s" C_RESET "  " C_YELLOW "unknown" C_RESET
             " -> " C_CYAN "%s" C_RESET "\n", targets[i], pb_full);
      to_update[nupdate++] = targets[i];
    } else if (version_compare(inst_ver, pb_full) >= 0) {
      printf("  " C_BOLD "%-24s" C_RESET "  " C_GREEN "up to date" C_RESET
             " (%s)\n", targets[i], pb_full);
      free(inst_ver);
    } else {
      printf("  " C_BOLD "%-24s" C_RESET "  " C_YELLOW "%s" C_RESET
             " -> " C_CYAN "%s" C_RESET "\n", targets[i], inst_ver, pb_full);
      to_update[nupdate++] = targets[i];
      free(inst_ver);
    }
  }

  printf("\n");
  if (nupdate == 0) {
    printf(C_CYAN "::" C_RESET " " C_GREEN "All packages up to date." C_RESET "\n");
    return;
  }

  printf("Packages to update (" C_BOLD "%d" C_RESET "):\n", nupdate);
  for (int i = 0; i < nupdate; i++) printf("  %s\n", to_update[i]);
  printf("\n");

  if (!flags.no_confirm)
    if (!confirm(C_CYAN "::" C_RESET " Proceed with rebuild? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
      printf(C_YELLOW "Operation cancelled." C_RESET "\n");
      return;
    }

  for (int i = 0; i < nupdate; i++) {
    CHECK_CANCEL(update_done);
    printf("\n" C_BOLD "==> Updating %s" C_RESET "\n", to_update[i]);
    /* wipe the build cache so it rebuilds from scratch */
    char cache[MAX_STR], rm_cmd[MAX_STR];
    snprintf(cache,  sizeof(cache),  "%s/%s", LPM_BUILD_DIR, to_update[i]);
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
    run(rm_cmd);
    char *pair[1] = { to_update[i] };
    cmd_sync(1, pair);
    lpm_log("Updated %s", to_update[i]);
  }
update_done:
  printf("\n" C_CYAN "::" C_RESET " " C_GREEN "Update complete." C_RESET "\n");
}

/* ── stubs for the Package API (used by transaction.c / merge.c) ─────── */
int pkg_build(Package *pkg, const LpmConfig *cfg) {
  TRACE("pkg_build(%s-%s)", pkg->name, pkg->version);
  DBG(1, "building %s-%s-%s", pkg->name, pkg->version, pkg->release);
  (void)pkg; (void)cfg; return 0;
}
int pkg_run_check(Package *pkg)              { (void)pkg; return 0; }
int pkg_run_package(Package *pkg)            { (void)pkg; return 0; }
int pkg_run_hook(const char *h, Package *pkg){ (void)h; (void)pkg; return 0; }

/* ══════════════════════════════════════════════════════════════════════
 * fetch_all_sources — batch-download all sources for the build queue
 *
 * Two-pass design:
 *   Pass 1: count total source files to pre-allocate the FetchJob array.
 *   Pass 2: for each package in the queue, build FetchJob entries for
 *           sources that are not already cached locally.
 *
 * Sources are skipped if:
 *   - the file already exists on disk and is large enough (>200 KiB),
 *     with a tar integrity check for archive files.
 *   - the file exists under /sources/ (offline mirror).
 *
 * After all downloads complete, checksums are verified for every package.
 * Returns 0 on success, -1 on download failure or checksum mismatch.
 * ══════════════════════════════════════════════════════════════════════ */
static int fetch_all_sources(char queue[][MAX_STR], int nqueue) {
  /* pass 1: count sources to size the FetchJob array */
  int total_srcs = 0;
  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) continue;
    for (int i = 0; i < pkg.nsources; i++)
      if (pkg.source[i][0]) total_srcs++;
  }
  if (total_srcs == 0) return 0;

  FetchJob *jobs = calloc(total_srcs, sizeof(FetchJob));
  if (!jobs) return -1;
  int njobs = 0;

  /* pass 2: build FetchJob list, skipping already-cached files */
  for (int qi = 0; qi < nqueue; qi++) {
    if (g_cancel) { free(jobs); return -1; }

    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) continue;
    if (db_is_installed(queue[qi])) continue;

    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);
    mkdir(ws, 0755);

    for (int i = 0; i < pkg.nsources; i++) {
      if (!pkg.source[i][0]) continue;
      char *fname = strrchr(pkg.source[i], '/');
      if (!fname) continue;
      fname++;

      char dest[MAX_STR];
      snprintf(dest, sizeof(dest), "%s/%s", ws, fname);

      struct stat st;
      /* skip if already downloaded and large enough */
      if (stat(dest, &st) == 0 && st.st_size >= (200 * 1024)) {
        if (strstr(fname, ".tar")) {
          char chk[MAX_STR];
          snprintf(chk, sizeof(chk), "tar -tf '%s' &>/dev/null", dest);
          if (system(chk) == 0) continue;
        } else
          continue;
      }

      /* try local offline mirror first */
      char local_src[MAX_STR];
      snprintf(local_src, sizeof(local_src), "/sources/%s", fname);
      if (stat(local_src, &st) == 0 && st.st_size > 0) {
        char cp_cmd[MAX_STR];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'", local_src, dest);
        system(cp_cmd);
        continue;
      }

      /* enqueue for download */
      FetchJob *j = &jobs[njobs++];
      strncpy(j->url,  pkg.source[i], LPM_URL_MAX  - 1);
      strncpy(j->dest, dest,          LPM_PATH_MAX - 1);
      char label[LPM_NAME_MAX];
      snprintf(label, sizeof(label), "%s-%s", pkg.pkgname, pkg.pkgver);
      strncpy(j->filename, label, LPM_NAME_MAX - 1);

      if (pkg.sha256sums[i][0] && strcmp(pkg.sha256sums[i], "SKIP") != 0) {
        strncpy(j->checksum, pkg.sha256sums[i], 128);
        j->cksum_type = CKSUM_SHA256;
      } else if (pkg.md5sums[i][0] && strcmp(pkg.md5sums[i], "SKIP") != 0) {
        strncpy(j->checksum, pkg.md5sums[i], 32);
        j->cksum_type = CKSUM_MD5;
      } else {
        j->cksum_type = CKSUM_SKIP;
      }
    }
  }

  int ret = (njobs > 0) ? dl_fetch_all(jobs, njobs) : 0;
  free(jobs);

  /* verify checksums after all downloads complete */
  if (ret == 0) {
    for (int qi = 0; qi < nqueue; qi++) {
      char pbfile[MAX_STR];
      snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
      Pkg pkg;
      if (pkgbuild_parse(pbfile, &pkg) != 0) continue;
      if (db_is_installed(queue[qi])) continue;

      char ws[MAX_STR];
      snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);

      if (!verify_sources(&pkg, ws)) {
        fprintf(stderr, C_RED "error: " C_RESET
                "checksum mismatch for %s — aborting\n", queue[qi]);
        ret = -1;
        break;
      }
    }
  }

  return ret;
}
