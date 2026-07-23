#include "lpm.h"
#include "llpm/llpm.h"
#include "llpm/trans.h"
#include "llpm/handle.h"
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* ══════════════════════════════════════════════════════════════════════
 * build.c — Build, sync, remove, and update commands for lpm
 *
 * Commands implemented here:
 *   lpm install <pkg>   fetch PKGBUILD + resolve deps + build + install
 *   lpm test    <pkg>   run check() test suite for an already-built package
 *   lpm remove  <pkg>   remove an installed package
 * ══════════════════════════════════════════════════════════════════════ */

/* g_cancel is set by the SIGINT handler in main.c */
extern volatile sig_atomic_t g_cancel;

/* Cancel-aware wrapper around system(). Returns -1 if g_cancel is set
 * or the child was killed by an interrupt signal, otherwise returns the
 * child exit code via WEXITSTATUS.
 *
 * system() makes the parent ignore SIGINT/SIGQUIT while the child runs,
 * so main.c's handler never fires for a Ctrl-C during e.g. `make`. We
 * detect that case here by inspecting the raw wait status instead of
 * blindly calling WEXITSTATUS (which would silently read back 0/success
 * for a signal-killed child). */
static int run(const char *cmd) {
  if (g_cancel)
    return -1;
  int rc = system(cmd);
  if (rc == -1)
    return -1;
  if (WIFSIGNALED(rc)) {
    int sig = WTERMSIG(rc);
    if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT || sig == SIGHUP) {
      if (!g_cancel) {
        g_cancel = 1;
        fprintf(stderr, "\n" C_YELLOW "warning:" C_RESET
                " Interrupt received — operation aborted.\n");
      }
    }
    return -1;
  }
  if (!WIFEXITED(rc))
    return -1;
  return WEXITSTATUS(rc);
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
int fetch_all_sources(char queue[][MAX_STR], int nqueue);

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

  /* Operation flags must never be treated as package names.
   * Handles accidental repetition like: lpm install -S firefox
   * sub_argv would be ["-S", "firefox"] — discard the extra -S. */
  static const char *OP_FLAGS[] = {
    "-S", "--sync", "-R", "--remove", "-Q", "--query",
    "-D", "--database", "-U", "--upgrade", "-F", "--files",
    NULL
  };

  int n = 0;
  for (int i = 0; i < argc; i++) {
    /* silently skip operation flags that leaked into sub_argv */
    int is_op = 0;
    for (int k = 0; OP_FLAGS[k]; k++)
      if (!strcmp(argv[i], OP_FLAGS[k])) { is_op = 1; break; }
    if (is_op) continue;

    if      (!strcmp(argv[i], "--force"))          f->force          = 1;
    else if (!strcmp(argv[i], "--no-confirm"))     f->no_confirm     = 1;
    else if (!strcmp(argv[i], "--strict"))         f->strict         = 1;
    else if (!strcmp(argv[i], "--no-check"))       f->no_check       = 1;
    else if (!strcmp(argv[i], "--no-recommended")) f->no_recommended = 1;
    else if (!strcmp(argv[i], "--recursive") || !strcmp(argv[i], "-Rs"))
      f->recursive = 1;
    else if (!strncmp(argv[i], "--debug=", 8)) {
      int lvl = atoi(argv[i] + 8);
      if (lvl < 1) lvl = 1;
      if (lvl > 3) lvl = 3;
      f->debug = lvl;
      g_debug  = lvl;
    }
    else if (!strcmp(argv[i], "--debug")) {
      f->debug = 1; g_debug = 1;
    }
    else if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-n"))
      f->dry_run = 1;
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
  char part[MAX_STR + 8];
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

/* ── checksum_for_source ─────────────────────────────────────────────── *
 * sources[i].checksum is "sha512:hex" / "sha256:hex" / "md5:hex" / "SKIP"
 * — the only checksum format LPDF v1 supports (see include/lpm.h's
 * Package struct / SPEC.md). parse_pkgbuild_c() already validated this
 * at parse time (an invalid format fails the whole parse), so by the
 * time this runs every source has exactly one checksum entry, always
 * valid. Returns the hex hash and sets *type_out, or returns NULL
 * (type CKSUM_SKIP) for an explicit "SKIP" entry.                       */
static const char *checksum_for_source(Package *pkg, int i, CksumType *type_out) {
    static char hash_buf[200];
    *type_out = CKSUM_SKIP;

    if (i < 0 || i >= pkg->nsources || !pkg->sources[i].checksum[0])
        return NULL;

    CksumType ct = checksum_parse_unified(pkg->sources[i].checksum,
                                           hash_buf, sizeof(hash_buf));
    if (ct == CKSUM_SKIP || !hash_buf[0])
        return NULL;

    *type_out = ct;
    return hash_buf;
}

/* ── verify_sources ──────────────────────────────────────────────────── *
 * Validates checksums for all sources declared in pkg against files     *
 * already downloaded into workspace directory ws. Delegates the actual  *
 * hashing to cksum_verify() (checksum.c) instead of re-invoking         *
 * sha*sum/md5sum here — one implementation of "hash a file and compare" *
 * for the whole codebase.                                               *
 * Returns 1 if every source with a checksum matches (SKIP entries       *
 * don't count against this), 0 on any mismatch.                         */
static int verify_sources(Package *pkg, const char *ws) {
  int ok = 1;

  for (int i = 0; i < pkg->nsources; i++) {
    char *fname = strrchr(pkg->sources[i].url, '/');
    fname = fname ? fname + 1 : pkg->sources[i].url;

    CksumType ct;
    const char *expected = checksum_for_source(pkg, i, &ct);
    if (!expected)
      continue;  /* explicit SKIP for this source */

    char filepath[MAX_STR];
    snprintf(filepath, sizeof(filepath), "%s/%s", ws, fname);

    if (cksum_verify(filepath, expected, ct) != 0)
      ok = 0;
    else
      printf("  ok [%s] %s\n",
             ct == CKSUM_SHA512 ? "sha512" :
             ct == CKSUM_SHA256 ? "sha256" : "md5",
             fname);
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
 * Given a parsed Package and its PKGBUILD path, this function:
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
            if (kl > 0 && kl < 63) { memcpy(key, p, (size_t)kl); key[kl] = '\0'; }

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

void do_build_install(Package *pkg, const char *pbfile_orig, LpmConfig *cfg,
                             int qi, int nqueue, const LpmFlags *flags) {
  /* Convert new-format PKGBUILD to bash-compatible temp file if needed */
  char *_converted_pb = pkgbuild_to_bash(pbfile_orig);
  const char *pbfile  = _converted_pb ? _converted_pb : pbfile_orig;
#define PBCLEAN() do { if (_converted_pb) { unlink(_converted_pb); free(_converted_pb); _converted_pb = NULL; } } while(0)

  if (g_cancel) { PBCLEAN(); return; }

  char ws[MAX_STR];
  snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg->name);
  mkdir(ws, 0755);

  /* extract each source archive into the workspace */
  for (int i = 0; i < pkg->nsources; i++) {
    if (!pkg->sources[i].url[0]) continue;
    char *fname = strrchr(pkg->sources[i].url, '/');
    if (!fname) continue;
    fname++;

    char srcpath[MAX_STR * 2];
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
      printf("  -> extracting %s\n", fname);
    run(excmd);
  }

  char pkg_log[MAX_STR];
  pkg_log_path(pkg->name, pkg_log, sizeof(pkg_log));

  time_t t_start = time(NULL);

  printf("\nBuilding %s-%s-%s (%d/%d)...\n",
         pkg->name, pkg->version, pkg->release, qi + 1, nqueue);
  lpm_log("Building %s %s-%s", pkg->name, pkg->version, pkg->release);

  /* run build() function from PKGBUILD */
  char build_cmd[MAX_CMD];
  /* srcdir: the extracted source tree — <ws>/<pkgname>-<pkgver>
   * Falls back to ws itself if that subdir doesn't exist yet.
   * startdir: the PKGBUILD directory (ws root)                */
  char srcdir[MAX_STR * 2];
  snprintf(srcdir, sizeof(srcdir), "%s/%s-%s",
           ws, pkg->name, pkg->version);
  {
      struct stat _sd;
      if (stat(srcdir, &_sd) != 0 || !S_ISDIR(_sd.st_mode))
          snprintf(srcdir, sizeof(srcdir), "%s", ws);
  }

  snprintf(build_cmd, sizeof(build_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " SOURCE_DATE_EPOCH=\"%ld\""
           " srcdir=\"%s\" startdir=\"%s\" SRCDEST=\"%s\""
           " && build' > \"%s\" 2>&1",
           pbfile, srcdir,
           cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx,
           (long)t_start,
           srcdir, ws, ws,
           pkg_log);

  if (run(build_cmd) != 0) {
    if (g_cancel) return;
    fprintf(stderr, C_RED "error: " C_RESET
            "Build failed: %s\n  Phase: build()\n  Log: " C_CYAN "%s" C_RESET "\n",
            pkg->name, pkg_log);
    lpm_log("Build FAILED: %s", pkg->name);
    PBCLEAN(); exit(1);
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* optional check() phase */
  if (pkg->has_check && !flags->no_check) {
    printf(":: Running check()...\n");
    char check_cmd[MAX_CMD];
    snprintf(check_cmd, sizeof(check_cmd),
             "bash -c 'source \"%s\" && cd \"%s\""
             " && export srcdir=\"%s\" startdir=\"%s\""
             " && check' >> \"%s\" 2>&1",
             pbfile, srcdir, srcdir, ws, pkg_log);
    int rc = run(check_cmd);

    /* always show the last 40 lines of the log after check */
    char tail[MAX_STR + 32];
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
      printf("  check() passed\n");
    }
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* run package() to populate pkgdir (the staging directory) */
  char pkgdir[MAX_STR + 8];
  snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", ws);
  char mk_cmd[MAX_STR * 2 + 64];
  snprintf(mk_cmd, sizeof(mk_cmd), "rm -rf '%s' && mkdir -p '%s'", pkgdir, pkgdir);
  run(mk_cmd);

  printf("Staging %s...\n", pkg->name);
  lpm_log("Staging %s", pkg->name);

  char inst_cmd[MAX_CMD];
  snprintf(inst_cmd, sizeof(inst_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " pkgdir=\"%s\" srcdir=\"%s\" startdir=\"%s\""
           " SRCDEST=\"%s\" SOURCE_DATE_EPOCH=\"%ld\""
           " && package' >> \"%s\" 2>&1",
           pbfile, srcdir,
           cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx,
           pkgdir, srcdir, ws,
           ws, (long)t_start, pkg_log);

  if (run(inst_cmd) != 0) {
    if (g_cancel) { PBCLEAN(); return; }
    fprintf(stderr, C_RED "error: " C_RESET
            "Install failed for %s\n  Phase: package()\n  Log: " C_CYAN "%s" C_RESET "\n",
            pkg->name, pkg_log);
    lpm_log("Install FAILED: %s", pkg->name);
    PBCLEAN(); exit(1);
  }

  if (g_cancel) { PBCLEAN(); return; }

  /* safety net: package() reported success but staged nothing — most
   * often caused by an interrupted build. Refuse rather than silently
   * recording a phantom install with 0 files. */
  {
    char chk_cmd[MAX_STR + 48];
    snprintf(chk_cmd, sizeof(chk_cmd),
             "find '%s' -mindepth 1 \\( -type f -o -type l \\) -print -quit",
             pkgdir);
    FILE *chk = popen(chk_cmd, "r");
    int has_files = 0;
    if (chk) {
      char buf[8];
      if (fgets(buf, sizeof(buf), chk)) has_files = 1;
      pclose(chk);
    }
    if (!has_files) {
      fprintf(stderr, C_RED "error: " C_RESET
              "package() produced no files for %s — refusing to install an empty package\n"
              "  Log: " C_CYAN "%s" C_RESET "\n",
              pkg->name, pkg_log);
      lpm_log("Install FAILED: %s (empty pkgdir after package())", pkg->name);
      PBCLEAN(); exit(1);
    }
  }

  /* strip debug symbols from shared libs and executables unless keep_pkg is set */
  if (cfg->keep_pkg == 0) {
    char strip_cmd[MAX_STR];
    snprintf(strip_cmd, sizeof(strip_cmd),
             "find '%s' -type f \\( -name '*.so*' -o -perm /111 \\)"
             " -exec strip --strip-unneeded '{}' + 2>/dev/null || true",
             pkgdir);
    run(strip_cmd);
  }

  /* Stage this Package for the transaction layer directly — no bridge
   * struct. pkg->type is left as whatever parse_pkgbuild_c() set from
   * pkgtype= (defaulting to PKG_TYPE_SOURCE); it used to be
   * unconditionally overwritten to PKG_TYPE_SOURCE here regardless of
   * the declared type, which made safety.c's PKG_TYPE_BINARY disk-space
   * check unreachable for anything built this way. */
  snprintf(pkg->pkg_dir, LPM_PATH_MAX, "%s", pkgdir);
  pkg->reason = REASON_EXPLICIT;
  pkg->state  = PKG_STATE_STAGED;

  /* ── replaces= handling ─────────────────────────────────────────
   * If this package declares replaces=(oldpkg ...) and the old
   * package is currently installed, auto-remove it first so the
   * new name takes over cleanly.  Print a clear notice so the user
   * knows why a package they never asked to remove disappeared.    */
  /* ── pack_only mode: skip install, leave pkgdir intact for packing ── */
  if (flags->pack_only) {
    long elapsed = (long)(time(NULL) - t_start);
    printf(":: Built %s-%s-%s in %lds — pkgdir: %s\n",
           pkg->name, pkg->version, pkg->release, elapsed, pkgdir);
    lpm_log("Built (pack_only) %s %s-%s", pkg->name, pkg->version, pkg->release);
    PBCLEAN();
    return;
  }

  for (int ri = 0; ri < pkg->nreplaces; ri++) {
    const char *old_name = pkg->replaces[ri];
    if (!db_is_installed(old_name)) continue;
    char *old_ver = db_get_version(old_name);
    printf(":: %s replaces %s", pkg->name, old_name);
    if (old_ver) { printf(" (%s)", old_ver); free(old_ver); }
    printf("\n");
    lpm_log("replaces: %s supersedes %s", pkg->name, old_name);
    /* remove the old package from db + filesystem */
    char rm_cmd[MAX_CMD];
    snprintf(rm_cmd, sizeof(rm_cmd),
             "lpm remove --no-confirm \"%s\" 2>/dev/null || true", old_name);
    (void)system(rm_cmd);
  }

  Transaction *tx = tx_new();
  if (!tx) {
    fprintf(stderr, C_RED "error: " C_RESET
            "out of memory allocating transaction for %s\n", pkg->name);
    PBCLEAN(); exit(1);
  }

  if (tx_add_install(tx, pkg) != 0) { PBCLEAN(); tx_free(tx); exit(1); }

  Package *pkg_arr[1] = { pkg };
  if (safety_check_conflicts(pkg_arr, 1, "/") != 0 && !flags->force) {
    fprintf(stderr, "Cannot continue.\n");
    PBCLEAN(); tx_free(tx);
    exit(1);
  }

  if (tx_commit(tx, "/") != 0) {
    if (g_cancel) { PBCLEAN(); tx_free(tx); return; }
    fprintf(stderr, C_RED "error: " C_RESET
            "Install failed for %s — transaction rolled back\n", pkg->name);
    tx_free(tx);
    exit(1);
  }

  tx_free(tx);
  PBCLEAN();
  lpm_audit("install: %s %s-%s", pkg->name, pkg->version, pkg->release);

  long elapsed = (long)(time(NULL) - t_start);
  lpm_log("Installed %s %s-%s (build time: %lds)",
          pkg->name, pkg->version, pkg->release, elapsed);

  /* ── save reproducible build metadata ────────────────────────── */
  {
    BuildMeta bm;
    buildmeta_collect(pkg->name, pkg->version, pkg->release,
                      pkg->type == PKG_TYPE_BINARY, pkg->pkg_dir, &bm);
    buildmeta_save(pkg->name, &bm);
  }

  /* clean up extracted sources if keep_src is not set */
  if (!cfg->keep_src) {
    char src_path[MAX_STR + 8];
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
 * the in-process parse cache so the next dep_resolve uses fresh data.
 * ══════════════════════════════════════════════════════════════════════ */
/* REPO_BASE comes from lpm.h (single source of truth) */

static const char *FOLDERS[] = { "base", "extra", "lotus" };
#define NFOLDERS 3

/*
 * pkg_exists_in_repo — HTTP HEAD check across all repo folders.
 *
 * Returns the folder index (0/1/2) where the package was found,
 * or -1 if it does not exist in any folder.
 *
 * Uses a single HEAD request per folder (no download, no retry).
 * wget: --spider  curl: -I --fail
 * Both suppress all output; only the exit code is used.
 */
/* ── URL builders ────────────────────────────────────────────────────── *
 * Two layouts are supported simultaneously:                              *
 *                                                                        *
 *  Flat (legacy):   <repo>/<letter>/pkgbuild_<name>                     *
 *  Directory (new): <repo>/<letter>/<name>/PKGBUILD                     *
 *                                                                        *
 * Local cache always stores as: LPM_PKGBUILD_DIR/pkgbuild_<name>        *
 * (keeps existing tooling working unchanged)                             */
static char g_letter(const char *name) {
    char c = name[0];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c < 'a' || c > 'z')  c = '0';
    return c;
}

/* flat:      .../lotus/b/pkgbuild_btop    */
static void url_flat(char *out, size_t n, const char *repo, const char *name) {
    snprintf(out, n, "%s/%s/%c/pkgbuild_%s",
             REPO_BASE, repo, g_letter(name), name);
}
/* directory: .../extra/b/btop/PKGBUILD   */
static void url_dir(char *out, size_t n, const char *repo, const char *name) {
    snprintf(out, n, "%s/%s/%c/%s/PKGBUILD",
             REPO_BASE, repo, g_letter(name), name);
}

/* ── pkg_locate_from_db ───────────────────────────────────────────────── *
 * Local repo.db lookup — returns 0..2 (base/extra/lotus) or -1.          *
 * Called before any network I/O; eliminates 6 HEAD requests per package   *
 * for the common case (lpm update has been run at least once).            *
 * If ver_out is non-NULL, also copies the repo's "VER-REL" string for     *
 * this package — this is what lets ROUND 0 tell a genuinely up-to-date   *
 * on-disk PKGBUILD apart from a stale one without any extra network I/O. *
 * Uses parse_repo_db() (sync.c) — the one repo.db parser — instead of    *
 * re-scanning lines by hand.                                             */
static int pkg_locate_from_db(const char *name, char *ver_out, size_t ver_sz) {
    static const char *DB_NAMES[] = { "base", "extra", "lotus", NULL };
    if (ver_out && ver_sz) ver_out[0] = '\0';

    for (int i = 0; DB_NAMES[i]; i++) {
        char path[LPM_PATH_MAX];
        snprintf(path, sizeof(path), "/var/lib/lpm/db/%s.db", DB_NAMES[i]);

        static RepoEntry entries[4096];
        int n = parse_repo_db(path, DB_NAMES[i], entries, 4096);

        for (int j = 0; j < n; j++) {
            if (strcmp(entries[j].name, name) != 0) continue;
            if (ver_out && ver_sz)
                snprintf(ver_out, ver_sz, "%s", entries[j].version);
            return i;
        }
    }
    return -1;
}

/* ── pkgbuild_on_disk_is_stale ───────────────────────────────────────── *
 * Decides whether an on-disk PKGBUILD needs refreshing, using only data  *
 * already on disk (the file itself + the last-synced repo.db) — no      *
 * network I/O, so this preserves ROUND 0's whole reason for existing.    *
 *                                                                         *
 * repo_ver may be "" when the package isn't in the local db yet (not     *
 * synced). In that case we can't prove staleness, so we trust the cache  *
 * — same behaviour as before this fix, for that specific case.          *
 *                                                                         *
 * Uses pkgbuild_parse_fast(), which is already cache-aware and spawns    *
 * no bash process, so this check is cheap even for hundreds of deps.     */
static int pkgbuild_on_disk_is_stale(const char *pbfile, const char *repo_ver) {
    if (!repo_ver || !repo_ver[0]) return 0; /* unverifiable: trust cache */

    Package m;
    if (pkgbuild_parse_fast(pbfile, &m) != 0) {
        /* Can't even parse it — that alone is a strong staleness/corruption
         * signal, so ask the caller to refetch rather than build from a
         * file we can't read. */
        return 1;
    }
    if (!m.version[0]) return 1; /* no version info: don't trust it */

    char local_ver[LPM_VER_MAX + 16];
    if (m.release[0])
        snprintf(local_ver, sizeof(local_ver), "%s-%s", m.version, m.release);
    else
        snprintf(local_ver, sizeof(local_ver), "%s", m.version);

    return strcmp(local_ver, repo_ver) != 0;
}

/* ── pkgbuild_needs_fetch ────────────────────────────────────────────── *
 * Single source of truth for "does this package's on-disk PKGBUILD need
 * a (re)fetch". Every caller that builds a "which pkgbuilds are missing"
 * list before calling fetch_pkgbuilds_parallel() used to hand-roll its
 * own bare stat()-only check (present == trust forever) — which is how
 * the ROUND-0 staleness bug ended up duplicated across build_queue() and
 * cmd_install()'s dependency pass, not just the top-level fetch. Routing
 * all of them through one function means there is now exactly one place
 * that defines "stale", instead of three that can drift out of sync.    */
static int pkgbuild_needs_fetch(const char *name) {
    char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
    struct stat st;
    if (stat(pbf, &st) != 0 || st.st_size <= 16) return 1; /* absent/empty */

    char repo_ver[LPM_VER_MAX + 16];
    pkg_locate_from_db(name, repo_ver, sizeof(repo_ver));
    return pkgbuild_on_disk_is_stale(pbf, repo_ver);
}

/* result struct: which repo + which layout matched */
typedef struct { int folder; int is_dir; } PkgLoc;

static int head_ok(const char *url) {
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
      "wget -q --spider --timeout=10 --tries=1 '%s' 2>/dev/null"
      " || curl -s -I --fail --connect-timeout 10 --max-time 15 '%s'"
      "    -o /dev/null 2>/dev/null",
      url, url);
    return system(cmd) == 0;
}

/* Serial fallback used only when pkg is absent from local db.
 * Tries dir then flat layout across all 3 repos (6 requests max).
 * Returns 0 on success, -1 if not found anywhere. */
static int fetch_pkgbuild_serial(const char *name) {
    PkgLoc loc = { -1, 0 };
    for (int f = 0; f < NFOLDERS && loc.folder < 0; f++) {
        char url[MAX_STR];
        url_dir(url, sizeof(url), FOLDERS[f], name);
        DBG(2, "HEAD dir  [%s]: %s", FOLDERS[f], url);
        if (head_ok(url)) { loc.folder = f; loc.is_dir = 1; break; }
        url_flat(url, sizeof(url), FOLDERS[f], name);
        DBG(2, "HEAD flat [%s]: %s", FOLDERS[f], url);
        if (head_ok(url)) { loc.folder = f; loc.is_dir = 0; break; }
    }
    if (loc.folder < 0) return -1;

    char dest[MAX_STR], url[MAX_STR];
    snprintf(dest, sizeof(dest), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
    if (loc.is_dir) url_dir(url, sizeof(url), FOLDERS[loc.folder], name);
    else            url_flat(url, sizeof(url), FOLDERS[loc.folder], name);

    FetchJob job = {0};
    snprintf(job.url, sizeof(job.url), "%s", url);
    snprintf(job.dest, sizeof(job.dest), "%s", dest);
    job.cksum_type = CKSUM_SKIP;
    int ok = (dl_fetch_all(&job, 1) == 0 && job.result == 0);
    if (ok) {
        struct stat st;
        if (stat(dest, &st) != 0 || st.st_size < 16) ok = 0;
    }
    if (!ok) { remove(dest); return -1; }
    printf("  -> Found in "
           C_CYAN "[%s]" C_RESET " (%s)\n",
           FOLDERS[loc.folder], loc.is_dir ? "dir" : "flat");
    lpm_log("Fetched pkgbuild_%s from %s/%s", name, REPO_BASE, FOLDERS[loc.folder]);
    pkgbuild_invalidate_cache(name);
    dep_meta_cache_invalidate();
    return 0;
}

/* ── fetch_pkgbuilds_parallel ─────────────────────────────────────────── *
 * Replaces N×6 serial HEAD + N×1 serial GET with:                        *
 *   1. O(N) local db scan  → repo per package, zero network              *
 *   2. one dl_fetch_all() round: try dir-layout for all at once          *
 *   3. one dl_fetch_all() retry round: flat-layout for any that failed   *
 *   4. serial HEAD fallback only for packages absent from local db       *
 *                                                                         *
 * For N=10 packages, old code: ~60 sequential subprocess spawns.         *
 * New code: 2 parallel batch rounds + 0 HEAD requests (typical case).    *
 *                                                                         *
 * names[]:     array of package names to fetch                           *
 * n:           length of names[]                                          *
 * missing[]:   output — names of packages not found anywhere (caller     *
 *              reports "error: target not found")                         *
 * Returns number of packages successfully fetched.                        */
static int fetch_pkgbuilds_parallel(const char **names, int n,
                                    const char **missing, int *nmissing) {
    if (n == 0) { *nmissing = 0; return 0; }
    util_mkdirp(LPM_PKGBUILD_DIR, 0755);

    /* ── ROUND 0: local db lookup — skip already-on-disk + known repo ── *
     * "On disk" alone used to be treated as "fresh forever": a           *
     * PKGBUILD fetched once would never be re-validated, so a legacy or  *
     * since-corrected file could silently persist across every future    *
     * build. We now cross-check the on-disk file's own pkgver-pkgrel     *
     * against the last-synced repo.db (pure local comparison, still      *
     * zero network I/O) and only trust the cache when they agree.        *
     * had_stale[i] remembers which packages we're refreshing despite     *
     * already having *something* on disk, so a failed refresh later      *
     * doesn't delete a still-usable fallback copy (see ROUND 1/2).       */
    int  db_repo[256];
    int  need_net[256]; /* indices into names[] that need a fetch         */
    int  nnet = 0;
    int  had_stale[256] = {0};

    for (int i = 0; i < n && i < 256; i++) {
        char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, names[i]);
        struct stat st;
        int on_disk = (stat(pbf, &st) == 0 && st.st_size > 16);

        char repo_ver[LPM_VER_MAX + 16];
        db_repo[i] = pkg_locate_from_db(names[i], repo_ver, sizeof(repo_ver));

        if (on_disk && !pkgbuild_on_disk_is_stale(pbf, repo_ver)) {
            DBG(3, "pkgbuild_%s: cached on disk, up to date", names[i]);
            continue;
        }

        if (on_disk) {
            had_stale[i] = 1;
            DBG(1, "pkgbuild_%s: on-disk copy is stale (repo has %s) —"
                   " refreshing", names[i], repo_ver[0] ? repo_ver : "?");
        } else {
            DBG(3, "pkgbuild_%s: db_repo=%d", names[i], db_repo[i]);
        }
        need_net[nnet++] = i;
    }

    if (nnet == 0) { *nmissing = 0; return n; }

    /* ── ROUND 1: parallel dir-layout fetch for packages known in db ─── */
    FetchJob *dir_jobs = calloc(256, sizeof(FetchJob));
    if (!dir_jobs) die("OOM in fetch_pkgbuilds_parallel");
    int dir_idx[256]; /* maps dir_jobs[j] → names[] index */
    int ndir = 0;
    int fallback_head[256]; /* indices (into names[]) needing serial HEAD */
    int nhead = 0;

    for (int k = 0; k < nnet; k++) {
        int i = need_net[k];
        if (db_repo[i] < 0) {
            /* not in local db → serial HEAD fallback (db not synced yet) */
            fallback_head[nhead++] = i;
            continue;
        }
        FetchJob *j = &dir_jobs[ndir];
        memset(j, 0, sizeof(*j));
        url_dir(j->url, sizeof(j->url), FOLDERS[db_repo[i]], names[i]);
        snprintf(j->dest, sizeof(j->dest), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, names[i]);
        j->cksum_type = CKSUM_SKIP;
        j->slot  = ndir;
        j->total = nnet - nhead; /* approx for progress bar */
        dir_idx[ndir] = i;
        ndir++;
    }

    if (ndir > 0) {
        DBG(1, "parallel PKGBUILD fetch: %d packages (dir-layout)", ndir);
        dl_fetch_all(dir_jobs, ndir);
    }

    /* ── ROUND 2: flat-layout retry for dir-layout misses ──────────── */
    FetchJob *flat_jobs = calloc(256, sizeof(FetchJob));
    if (!flat_jobs) { free(dir_jobs); die("OOM in fetch_pkgbuilds_parallel"); }
    int flat_idx[256];
    int nflat = 0;
    int still_missing[256]; /* indices (into names[]) after both rounds */
    int nstill = 0;

    for (int k = 0; k < ndir; k++) {
        int i = dir_idx[k];
        /* success check: downloaded + non-trivial size */
        struct stat st;
        int ok = (dir_jobs[k].result == 0 &&
                  stat(dir_jobs[k].dest, &st) == 0 && st.st_size > 16);
        if (ok) {
            DBG(2, "pkgbuild_%s: dir-layout OK", names[i]);
            pkgbuild_invalidate_cache(names[i]);
            continue;
        }
        /* dl_worker() only ever replaces dest via rename-on-success, so a
         * failed attempt never touches a pre-existing file — but this
         * cleanup line used to remove(dest) unconditionally regardless.
         * That was harmless while ROUND 0 could only send *absent* files
         * here; now that a stale-but-present file can reach this point,
         * deleting it on a transient failure would trade a silent
         * staleness bug for a hard failure / data loss on a flaky
         * network. Only clean up when there was nothing worth keeping. */
        if (!had_stale[i]) remove(dir_jobs[k].dest); /* clean partial */
        /* retry with flat layout */
        FetchJob *j = &flat_jobs[nflat];
        memset(j, 0, sizeof(*j));
        url_flat(j->url, sizeof(j->url), FOLDERS[db_repo[i]], names[i]);
        snprintf(j->dest, sizeof(j->dest), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, names[i]);
        j->cksum_type = CKSUM_SKIP;
        j->slot  = nflat;
        j->total = nflat + 1; /* filled in as we go */
        flat_idx[nflat] = i;
        nflat++;
    }

    if (nflat > 0) {
        DBG(1, "parallel PKGBUILD retry: %d packages (flat-layout)", nflat);
        for (int k = 0; k < nflat; k++) flat_jobs[k].total = nflat;
        dl_fetch_all(flat_jobs, nflat);
    }

    for (int k = 0; k < nflat; k++) {
        int i = flat_idx[k];
        struct stat st;
        int ok = (flat_jobs[k].result == 0 &&
                  stat(flat_jobs[k].dest, &st) == 0 && st.st_size > 16);
        if (ok) {
            DBG(2, "pkgbuild_%s: flat-layout OK", names[i]);
            pkgbuild_invalidate_cache(names[i]);
        } else if (had_stale[i]) {
            /* Both refresh attempts failed, but we still have the old
             * on-disk copy (never deleted — see ROUND 1). Prefer building
             * with known-stale data over hard-failing the whole install;
             * this matches the pre-fix behaviour for this package (which
             * never even attempted a refresh) rather than introducing a
             * new failure mode purely because we can now detect staleness. */
            fprintf(stderr, C_YELLOW "warning: " C_RESET
                    "could not refresh pkgbuild_%s (network?) — using the "
                    "existing on-disk copy, which may be out of date\n",
                    names[i]);
        } else {
            remove(flat_jobs[k].dest);
            still_missing[nstill++] = i;
        }
    }

    /* ── ROUND 3: serial HEAD fallback for packages absent from db ── */
    for (int k = 0; k < nhead; k++) {
        int i = fallback_head[k];
        DBG(1, "pkgbuild_%s: not in local db, HEAD probing", names[i]);
        if (fetch_pkgbuild_serial(names[i]) != 0)
            still_missing[nstill++] = i;
    }

    /* ── collect missing ─────────────────────────────────────────── */
    *nmissing = 0;
    for (int k = 0; k < nstill; k++)
        missing[(*nmissing)++] = names[still_missing[k]];

    dep_meta_cache_invalidate();
    int nfetched = 0;
    for (int i = 0; i < n; i++) {
        char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, names[i]);
        struct stat st;
        if (stat(pbf, &st) == 0 && st.st_size > 16) nfetched++;
    }
    free(dir_jobs);
    free(flat_jobs);
    return nfetched;
}

/* Single-package convenience wrapper around fetch_pkgbuilds_parallel().
 * 6 call sites across build.c/lpkg.c — not legacy, not dead. */
int fetch_pkgbuild(const char *name) {
    const char *missing[1];
    int nm = 0;
    const char *names[1] = { name };
    fetch_pkgbuilds_parallel(names, 1, missing, &nm);
    return nm > 0 ? -1 : 0;
}

/* ── cmd_fetch (internal: PKGBUILD fetch) ─────────────────────────────────────────────────── */
void cmd_fetch(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm update <package>");
  for (int i = 0; i < argc; i++) {
    if (fetch_pkgbuild(argv[i]) != 0)
      fprintf(stderr, C_RED "error: " C_RESET "pkgbuild_%s not found\n", argv[i]);
  }
  printf(":: PKGBUILDs saved to %s\n"
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

/* pkg_patch_from_fast() removed: it existed to patch up incomplete
 * results from the old bash metadata parser using the fast C parser's
 * output. Every call site now calls pkgbuild_parse_fast() directly and
 * gets complete, correct fields the first time — nothing left to patch. */

static int build_queue(char **pkgs, int npkgs, char queue[][MAX_STR], int maxq) {
  /* collect all packages + transitive deps, toposort in one shot */
  int nqueue = dep_resolve_queue_multi(pkgs, npkgs, queue, maxq, 0);

  /* batch-fetch any PKGBUILDs missing from disk */
  const char *need[256];
  int nneed = 0;
  for (int j = 0; j < nqueue && nneed < 256; j++) {
    if (pkgbuild_needs_fetch(queue[j]))
      need[nneed++] = queue[j];
  }
  if (nneed > 0) {
    const char *miss[256]; int nm = 0;
    fetch_pkgbuilds_parallel(need, nneed, miss, &nm);
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
    die("No package specified.\nUsage: lpm installi <package>");

  /* verify all requested PKGBUILDs exist before doing any work */
  for (int i = 0; i < npkgs; i++) {
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgs[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0)
      die("PKGBUILD not found for '%s'\n  Use 'lpm install' to fetch.", pkgs[i]);
  }

  char queue[256][MAX_STR];
  int nqueue = build_queue(pkgs, npkgs, queue, 256);

  cmd_deptree(npkgs, pkgs);

  if (nqueue > 0) {
    printf(":: Will build %d package(s):\n",
           nqueue);
    for (int i = 0; i < nqueue; i++)
      printf("    %d. %s\n", i + 1, queue[i]);
    printf("\n");
  }

  if (!flags.no_confirm)
    if (!confirm(C_CYAN "::" C_RESET " Proceed with build? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] ")) {
      printf("Operation cancelled.\n");
      return;
    }

  if (fetch_all_sources(queue, nqueue) != 0)
    die("Source fetch failed.");

  for (int qi = 0; qi < nqueue; qi++) {
    CHECK_CANCEL(build_done);
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) {
      warn("No PKGBUILD for '%s', skipping", queue[qi]);
      continue;
    }
    if (db_is_installed(queue[qi])) {
      printf("%s already installed, skipping\n", queue[qi]);
      continue;
    }
    do_build_install(&pkg, pbfile, &cfg, qi, nqueue, &flags);

    if (!g_cancel && !flags.no_recommended)
      lpm_prompt_recommends(queue[qi], &flags, install_one);
  }
build_done:;
}

/* ── cmd_sync (install) ───────────────────────────────────────────────────── *
 * Fetch PKGBUILD + resolve deps + build + install.                      *
 * Full online sync: always fetches the PKGBUILD from the repo first.   */

/* ── format_size ─────────────────────────────────────────────────────── *
 * Format bytes → human-readable: "339.9 MiB", "1.2 GiB", "512 KiB"    */
void format_size(long bytes, char *out, size_t outsz) {
    if (bytes <= 0)              { snprintf(out, outsz, "?"); return; }
    if (bytes >= 1073741824L)
        snprintf(out, outsz, "%.1f GiB", bytes / 1073741824.0);
    else if (bytes >= 1048576L)
        snprintf(out, outsz, "%.1f MiB", bytes / 1048576.0);
    else if (bytes >= 1024L)
        snprintf(out, outsz, "%.1f KiB", bytes / 1024.0);
    else
        snprintf(out, outsz, "%ld B", bytes);
}

/* ── repo_db_get_size ────────────────────────────────────────────────── *
 * Look up dlsize and instsize for a package from the persisted repo.db. *
 * Returns 0 on success (fills *dl and *inst), -1 if not found.          *
 * Uses parse_repo_db() (sync.c) — the one repo.db parser.               */
static int repo_db_get_size(const char *pkgname, long *dl, long *inst) {
    static const char *DB_NAMES[] = { "base", "extra", "lotus", NULL };
    *dl = 0; *inst = 0;

    for (int ri = 0; DB_NAMES[ri]; ri++) {
        char path[LPM_PATH_MAX];
        snprintf(path, sizeof(path), "/var/lib/lpm/db/%s.db", DB_NAMES[ri]);

        static RepoEntry entries[4096];
        int n = parse_repo_db(path, DB_NAMES[ri], entries, 4096);

        for (int j = 0; j < n; j++) {
            if (strcmp(entries[j].name, pkgname) != 0) continue;
            *dl   = entries[j].dl_size;
            *inst = entries[j].inst_size;
            return 0;
        }
    }
    return -1;
}

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
    die("No package specified.\nUsage: lpm install <package>");

  /* ── group expansion: "lpm install xlib" → all group members ─────── */
  {
    static char _exp_buf[320][LPM_NAME_MAX];
    static char *_exp_ptrs[320];
    int _exp_n = 0;
    for (int _ai = 0; _ai < npkgs && _exp_n < 320; _ai++) {
      char _mem[256][LPM_NAME_MAX];
      int  _nm = cmd_group_expand(pkgs[_ai], _mem, 256);
      if (_nm > 0) {
        printf("Expanding group '%s' (%d packages)\n",
               pkgs[_ai], _nm);
        for (int _mi = 0; _mi < _nm && _exp_n < 320; _mi++) {
          strncpy(_exp_buf[_exp_n], _mem[_mi], LPM_NAME_MAX-1);
          _exp_ptrs[_exp_n] = _exp_buf[_exp_n];
          _exp_n++;
        }
      } else {
        strncpy(_exp_buf[_exp_n], pkgs[_ai], LPM_NAME_MAX-1);
        _exp_ptrs[_exp_n] = _exp_buf[_exp_n];
        _exp_n++;
      }
    }
    if (_exp_n != npkgs) {
      npkgs = _exp_n;
      for (int _i = 0; _i < npkgs; _i++) pkgs[_i] = _exp_ptrs[_i];
    }
  }

  /* ── --dry-run: preview, no side effects ── */
  if (flags.dry_run) {
    DryRun dr;
    dryrun_build(pkgs, npkgs, &dr);
    dryrun_print(&dr);
    return;
  }

  /* ══════════════════════════════════════════════════════════════════
   * STEP 1: Fetch PKGBUILDs for all requested packages in parallel.
   *   Old: N × (6 HEAD serial subprocesses + 1 GET)  → O(7N) serial
   *   New: local db lookup O(N) + 1-2 parallel dl_fetch_all() rounds
   * ══════════════════════════════════════════════════════════════════ */
  {
    const char *req[256];
    const char *missing[256];
    int nm = 0;
    for (int i = 0; i < npkgs && i < 256; i++) req[i] = pkgs[i];
    fetch_pkgbuilds_parallel(req, npkgs, missing, &nm);
    if (nm > 0) {
      for (int i = 0; i < nm; i++)
        fprintf(stderr, "Error:\n\n"
                "target not found: %s\n\n"
                "Cannot continue.\n",
                missing[i]);
      exit(1);
    }
  }

  /* ══════════════════════════════════════════════════════════════════
   * STEP 2: Resolve full dep queue (topo-sorted, skip installed)
   *   Then batch-fetch all missing dep PKGBUILDs in parallel.
   * ══════════════════════════════════════════════════════════════════ */
  printf("Resolving dependencies...\n\n");
  char queue[256][MAX_STR];
  int nqueue = build_queue(pkgs, npkgs, queue, 256);

  /* collect deps that need fetching, batch in one parallel round */
  {
    const char *need[256];
    int nneed = 0;
    for (int qi = 0; qi < nqueue && nneed < 256; qi++) {
      if (db_is_installed(queue[qi])) continue;
      if (pkgbuild_needs_fetch(queue[qi]))
        need[nneed++] = queue[qi];
    }
    if (nneed > 0) {
      const char *missing[256]; int nm = 0;
      fetch_pkgbuilds_parallel(need, nneed, missing, &nm);
      /* missing dep PKGBUILDs are non-fatal here — dep_resolve already
       * validated deps exist; a missing PKGBUILD means the dep is a
       * binary-only package (installed via .lpkg path), not an error. */
      for (int i = 0; i < nm; i++)
        DBG(2, "no PKGBUILD for dep %s (binary pkg?)", missing[i]);
    }
  }

  /* ══════════════════════════════════════════════════════════════════
   * STEP 3: Show transaction summary
   *   Packages (N):
   *     dep1  dep2  firefox
   *
   *   Recommended packages:
   *     ffmpeg - Multimedia codec support
   *     ...
   *   Would you like to install recommended packages? [Y/n]
   * ══════════════════════════════════════════════════════════════════ */
  if (nqueue == 0) {
    printf("There is nothing to do.\n");
    return;
  }

  /* ── Transaction summary ────────────────────────────────────────── */
  {
    long total_dl = 0, total_inst = 0;
    int  n_new = 0;

    /* first pass: count and accumulate sizes */
    for (int i = 0; i < nqueue; i++) {
      if (db_is_installed(queue[i])) continue;
      n_new++;
      char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
               LPM_PKGBUILD_DIR, queue[i]);
      Package fm; memset(&fm, 0, sizeof(fm));
      pkgbuild_parse_fast(pbf, &fm);
      long dl = 0, inst = 0;
      if (repo_db_get_size(queue[i], &dl, &inst) != 0) {
        dl   = fm.dl_size;
        inst = fm.inst_size;
      }
      total_dl   += dl;
      total_inst += inst;
    }

    if (n_new == 0) {
      /* all targets already installed — report first explicit target */
      char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
               LPM_PKGBUILD_DIR, pkgs[0]);
      Package fm; memset(&fm, 0, sizeof(fm));
      pkgbuild_parse_fast(pbf, &fm);
      if (fm.version[0])
        printf("%s-%s is already installed.\n", pkgs[0], fm.version);
      else
        printf("%s is already installed.\n", pkgs[0]);
      return;
    }

    printf("Packages (%d)\n\n", n_new);

    for (int i = 0; i < nqueue; i++) {
      if (db_is_installed(queue[i])) continue;
      char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
               LPM_PKGBUILD_DIR, queue[i]);
      Package fm; memset(&fm, 0, sizeof(fm));
      pkgbuild_parse_fast(pbf, &fm);
      if (fm.version[0])
        printf("%s-%s\n", queue[i], fm.version);
      else
        printf("%s\n", queue[i]);
    }

    if (total_dl > 0 || total_inst > 0) {
      printf("\n");
      char sdl[24], sinst[24];
      format_size(total_dl,   sdl,   sizeof(sdl));
      format_size(total_inst, sinst, sizeof(sinst));
      if (total_dl   > 0) printf("Download size: %s\n", sdl);
      if (total_inst > 0) printf("Installed size: %s\n", sinst);
    }
    printf("\n");
  }

  /* ── collect recommends from ALL packages in queue ── */
  char rec_names[64][LPM_NAME_MAX];
  char rec_descs[64][512];
  int  nrec = 0;

  if (!flags.no_recommended) {
    for (int qi = 0; qi < nqueue && nrec < 64; qi++) {
      if (db_is_installed(queue[qi])) continue;
      char rec_tmp[LPM_MAX_DEPS][MAX_STR];
      int  nr = dep_get_recommends(queue[qi], rec_tmp, LPM_MAX_DEPS);
      for (int r = 0; r < nr && nrec < 64; r++) {
        /* skip if already installed or already in list */
        if (db_is_installed(rec_tmp[r])) continue;
        int dup = 0;
        for (int k = 0; k < nrec; k++)
          if (!strcmp(rec_names[k], rec_tmp[r])) { dup = 1; break; }
        if (dup) continue;

        snprintf(rec_names[nrec], LPM_NAME_MAX, "%s", rec_tmp[r]);

        /* fetch description from PKGBUILD */
        rec_descs[nrec][0] = '\0';
        char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, rec_tmp[r]);
        Package rm; memset(&rm, 0, sizeof(rm));
        if (pkgbuild_parse_fast(pbf, &rm) == 0 && rm.description[0])
          snprintf(rec_descs[nrec], 512, "%s", rm.description);

        nrec++;
      }
    }
  }

  /* display recommends and ask once */
  int install_recommends = 0;
  if (nrec > 0) {
    printf("Recommended packages:\n\n");
    for (int r = 0; r < nrec; r++) {
      if (rec_descs[r][0])
        printf("%s - %s\n", rec_names[r], rec_descs[r]);
      else
        printf("%s\n", rec_names[r]);
    }
    printf("\n");

    if (flags.no_confirm)
      install_recommends = 1;
    else
      install_recommends = confirm("Install recommended packages? [Y/n] ");
    printf("\n");
  }

  /* final confirm */
  if (!flags.no_confirm) {
    if (!confirm("Proceed? [Y/n] "))
      return;
  }
  printf("\n");

  /* ══════════════════════════════════════════════════════════════════
   * STEP 4: "Buy everything at the market first"
   *   Download ALL sources for deps + optional recommends before
   *   building anything.  Zero partial-build failures from net issues.
   * ══════════════════════════════════════════════════════════════════ */

  /* build full download queue = deps + accepted recommends */
  char dl_queue[320][MAX_STR];
  int  ndl = 0;
  for (int i = 0; i < nqueue && ndl < 320; i++)
    snprintf(dl_queue[ndl++], MAX_STR, "%s", queue[i]);

  if (install_recommends) {
    /* resolve recommends into their own dep queues and append */
    char *rnames[64];
    for (int r = 0; r < nrec; r++) rnames[r] = rec_names[r];
    char rec_queue[64][MAX_STR];
    int nrq = dep_resolve_queue_multi(rnames, nrec, rec_queue, 64, 0);
    for (int r = 0; r < nrq && ndl < 320; r++) {
      /* don't duplicate */
      int dup = 0;
      for (int k = 0; k < ndl; k++)
        if (!strcmp(dl_queue[k], rec_queue[r])) { dup = 1; break; }
      if (!dup)
        snprintf(dl_queue[ndl++], MAX_STR, "%s", rec_queue[r]);
    }
    /* also fetch their PKGBUILDs */
    for (int r = 0; r < nrq; r++) {
      if (pkgbuild_needs_fetch(rec_queue[r]))
        fetch_pkgbuild(rec_queue[r]);
    }
  }

  /* ══════════════════════════════════════════════════════════════════
   * STEP 4 / 5: binary fast-path OR source download + build
   *
   * For each package in dl_queue:
   *   - If PKGBUILD has pkgtype=binary (or pkgtype=bin):
   *       → try to download <name>-<ver>-<rel>-<arch>.lpkg from repo
   *       → if found: install with lpkg_install_from_file(), skip build
   *       → if not found: fall through to source build (graceful)
   *   - Otherwise: classic fetch_all_sources + do_build_install
   * ══════════════════════════════════════════════════════════════════ */

  /* Separate queue into binary and source lists */
  char src_queue[320][MAX_STR];
  int  nsrc = 0;

  for (int qi = 0; qi < ndl; qi++) {
    CHECK_CANCEL(sync_done);
    const char *pkgname = dl_queue[qi];

    if (db_is_installed(pkgname)) {
      continue;
    }

    /* parse to check pkgtype */
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
             LPM_PKGBUILD_DIR, pkgname);

    Package fast_meta;
    memset(&fast_meta, 0, sizeof(fast_meta));
    pkgbuild_parse_fast(pbfile, &fast_meta);

    if ((fast_meta.type == PKG_TYPE_BINARY)) {
      /* ── binary fast-path ────────────────────────────────────────── */
      /* filename convention: <name>-<ver>-<rel>-<arch>.lpkg            */
      /* arch: uname -m at build time; default to x86_64               */
      char arch[32] = "x86_64";
      FILE *fp = popen("uname -m 2>/dev/null", "r");
      if (fp) { if (fgets(arch, sizeof(arch), fp)) {
          char *nl = strchr(arch, '\n'); if (nl) *nl = '\0'; } pclose(fp); }

      /* build URL: REPO_BASE/<repo>/<letter>/<name>-<ver>-<rel>-<arch>.lpkg */
      char letter = pkgname[0];
      if (letter >= 'A' && letter <= 'Z') letter += 32;
      if (letter < 'a' || letter > 'z')  letter = '0';

      char lpkg_fname[LPM_PATH_MAX];
      snprintf(lpkg_fname, sizeof(lpkg_fname),
               "%s-%s-%s-%s.lpkg",
               pkgname,
               fast_meta.version[0] ? fast_meta.version : "0",
               fast_meta.release[0] ? fast_meta.release : "1",
               arch);

      char lpkg_url[LPM_URL_MAX];
      /* try each repo in order: base → extra → lotus */
      static const char *_bin_repos[] = { "base", "extra", "lotus", NULL };
      char lpkg_dest[LPM_PATH_MAX];
      snprintf(lpkg_dest, sizeof(lpkg_dest),
               "/var/cache/lpm/packages/%s", lpkg_fname);

      int bin_ok = 0;
      for (int ri = 0; _bin_repos[ri] && !bin_ok; ri++) {
        snprintf(lpkg_url, sizeof(lpkg_url),
                 "%s/%s/%c/%s",
                 REPO_BASE, _bin_repos[ri], letter, lpkg_fname);

        char part[LPM_PATH_MAX];
        snprintf(part, sizeof(part), "%s.part", lpkg_dest);

        printf("Downloading...\n\n%s\n\n", lpkg_fname);

        char dl_cmd[2048];
        snprintf(dl_cmd, sizeof(dl_cmd),
            "wget -q --timeout=120 --tries=2 --show-progress"
            " -O '%s' '%s' 2>/dev/null"
            " || curl -L --connect-timeout 30 --max-time 300"
            " --progress-bar -o '%s' '%s' 2>/dev/null",
            part, lpkg_url, part, lpkg_url);

        struct stat bst;
        if (system(dl_cmd) == 0 &&
            stat(part, &bst) == 0 && bst.st_size > 512) {
          rename(part, lpkg_dest);
          bin_ok = 1;
        } else {
          remove(part);
        }
      }

      if (bin_ok) {
        printf("Installing %s...\n", pkgname);
        fflush(stdout);
        int _bin_rc = lpkg_install_from_file(lpkg_dest);
        if (_bin_rc != 0)
          fprintf(stderr, "Error:\n\n"
                  "install failed: %s\n\n"
                  "Cannot continue.\n", pkgname);
        lpm_log("Installed %s (binary) from repo", pkgname);
        continue;
      }

      /* binary not found in any repo — fall through to source build */
      warn("binary for %s not found in repo, falling back to source build",
           pkgname);
    }

    /* ── source build: add to src_queue ─────────────────────────── */
    snprintf(src_queue[nsrc++], MAX_STR, "%s", pkgname);
  }

  /* Download all source tarballs in one batch */
  if (nsrc > 0) {
    printf("Downloading...\n\n");
    if (fetch_all_sources(src_queue, nsrc) != 0)
      die("Source download failed — aborting before any build.");

    /* Build + install source packages in topo order */
    for (int qi = 0; qi < nsrc; qi++) {
      CHECK_CANCEL(sync_done);
      char pbfile2[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbfile2, sizeof(pbfile2), "%s/pkgbuild_%s",
               LPM_PKGBUILD_DIR, src_queue[qi]);

      Package pkg;
      if (pkgbuild_parse_fast(pbfile2, &pkg) != 0) {
        warn("No PKGBUILD for '%s', skipping", src_queue[qi]);
        continue;
      }

      if (db_is_installed(src_queue[qi]))
        continue;
      printf("Installing %s...\n", src_queue[qi]);
      do_build_install(&pkg, pbfile2, &cfg, qi, nsrc, &flags);
    }
  }

sync_done:;
  if (g_cancel)
    printf("\nAborted.\n");
  else
    printf("\nDone.\n");
}

/* ── cmd_check (test) ─────────────────────────────────────────────────── *
 * Run the check() test suite for a package that has already been built. *
 * Does not install anything; only runs the test phase.                  */
void cmd_check(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm installc <package>");

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgargs[256];
  int nargs = lpm_parse_flags(argc, argv, &flags, pkgargs, 256);
  if (nargs == 0) { pkgargs[0] = NULL; }
  (void)pkgargs;

  for (int i = 0; i < argc; i++) {
    CHECK_CANCEL(check_done);
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, argv[i]);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0)
      die("PKGBUILD not found for '%s'", argv[i]);

    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.name);

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
    pkg_log_path(pkg.name, pkg_log, sizeof(pkg_log));

    printf(":: Running check() for %s...\n",
           argv[i]);
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
             "bash -c 'source \"%s\" && cd \"%s\" && check' > \"%s\" 2>&1",
             pbfile, ws, pkg_log);
    int rc = run(cmd);

    char tail[MAX_STR + 32];
    snprintf(tail, sizeof(tail), "tail -40 '%s'", pkg_log);
    run(tail);

    if (rc == 0) {
      printf("check() passed\n");
      lpm_log("check() passed for %s", argv[i]);
    } else {
      printf("check() FAILED (rc=%d)"
             "  Log: " C_CYAN "%s" C_RESET "\n", rc, pkg_log);
      lpm_log("check() failed for %s (rc=%d)", argv[i], rc);
      if (!flags.no_confirm)
        if (!confirm(C_CYAN "::" C_RESET " check() failed. Continue anyway? [" C_GREEN "Y" C_RESET "/" C_RED "n" C_RESET "] "))
          { printf("Operation cancelled.\n"); exit(1); }
    }
  }
check_done:;
}

/* ── helpers for recursive remove / reverse-dep checks ───────────────── */

static int remove_set_contains(char **set, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (set[i] && !strcmp(set[i], name)) return 1;
  return 0;
}

/* Collect packages outside `set` that depend on `target`. */
static int collect_breakers(const char *target, char **set, int nset,
                            char out[][LPM_NAME_MAX], int maxout) {
  char *rdeps = reverse_deps(target);
  int n = 0;
  if (!rdeps || !rdeps[0]) return 0;

  char buf[LPM_NAME_MAX * 256];
  snprintf(buf, sizeof(buf), "%s", rdeps);
  char *save = NULL;
  for (char *tok = strtok_r(buf, " \t", &save);
       tok && n < maxout;
       tok = strtok_r(NULL, " \t", &save)) {
    if (remove_set_contains(set, nset, tok)) continue;
    snprintf(out[n++], LPM_NAME_MAX, "%s", tok);
  }
  return n;
}

/* Expand remove set with unused deps (REASON_DEP, not required outside set). */
static int expand_unused_deps(char **pkgs, int *npkgs, int maxpkgs,
                              char storage[][LPM_NAME_MAX]) {
  int changed = 1;
  while (changed) {
    changed = 0;
    int ncur = *npkgs;
    for (int i = 0; i < ncur && *npkgs < maxpkgs; i++) {
      char pbf[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s",
               LPM_PKGBUILD_DIR, pkgs[i]);
      Package meta; memset(&meta, 0, sizeof(meta));
      if (pkgbuild_parse_fast(pbf, &meta) != 0) continue;

      for (int d = 0; d < meta.ndepends && *npkgs < maxpkgs; d++) {
        char depname[LPM_NAME_MAX];
        snprintf(depname, sizeof(depname), "%s", meta.depends[d]);
        char *op = strpbrk(depname, "><=");
        if (op) *op = '\0';
        if (!depname[0] || !db_is_installed(depname)) continue;
        if (remove_set_contains(pkgs, *npkgs, depname)) continue;

        InstalledPkg ip; memset(&ip, 0, sizeof(ip));
        if (db_query(depname, &ip) != 0) continue;
        if (ip.reason != REASON_DEP) continue;

        /* still required by something outside the remove set? */
        char breakers[64][LPM_NAME_MAX];
        int nb = collect_breakers(depname, pkgs, *npkgs, breakers, 64);
        if (nb > 0) continue;

        snprintf(storage[*npkgs], LPM_NAME_MAX, "%s", depname);
        pkgs[*npkgs] = storage[*npkgs];
        (*npkgs)++;
        changed = 1;
      }
    }
  }
  return 0;
}

/* ── cmd_remove (remove) ─────────────────────────────────────────────────── *
 * Remove one or more installed packages.                                *
 *                                                                        *
 * Safety checks:                                                        *
 *   - reverse dependency check (blocks unless --force or -Rs)           *
 *   - CriticalPkg protection (--force required)                         *
 *   - installed check                                                   *
 *                                                                        *
 * -Rs / --recursive also removes unused dependencies of the targets.    *
 * ─────────────────────────────────────────────────────────────────────── */
void cmd_remove(int argc, char **argv) {
  check_root();
  init_dirs();
  check_remove_journal();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[256];
  static char pkg_storage[256][LPM_NAME_MAX];
  memset(pkgs, 0, sizeof(pkgs));
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 255);

  if (npkgs == 0)
    die("No package specified.\nUsage: lpm remove <package(s)>");

  /* copy names into stable storage (recursive expand needs it) */
  int nexplicit = npkgs;
  for (int i = 0; i < npkgs; i++) {
    snprintf(pkg_storage[i], LPM_NAME_MAX, "%s", pkgs[i]);
    pkgs[i] = pkg_storage[i];
  }

  /* ── 1. verify all packages are installed ───────────────────────── */
  for (int i = 0; i < npkgs; i++) {
    if (!db_is_installed(pkgs[i])) {
      fprintf(stderr,
              "Error:\n\n"
              "'%s' is not installed\n\n"
              "Cannot continue.\n",
              pkgs[i]);
      exit(1);
    }
  }

  /* ── dry-run ────────────────────────────────────────────────────── */
  if (flags.dry_run) {
    if (flags.recursive)
      expand_unused_deps(pkgs, &npkgs, 256, pkg_storage);
    DryRun dr;
    dryrun_remove(pkgs, npkgs, &dr);
    dryrun_print(&dr);
    return;
  }

  /* ── 2. critical guard ──────────────────────────────────────────── */
  {
    char crit[64][LPM_NAME_MAX];
    int ncrit = 0;
    for (int i = 0; i < npkgs && ncrit < 64; i++) {
      if (lpm_config_is_critical(&cfg, pkgs[i]))
        snprintf(crit[ncrit++], LPM_NAME_MAX, "%s", pkgs[i]);
    }
    if (ncrit > 0 && !flags.force) {
      fprintf(stderr, "Error:\n\n");
      fprintf(stderr, "Refusing to remove critical packages.\n\n");
      fprintf(stderr, "Requested:\n\n");
      for (int i = 0; i < ncrit; i++)
        fprintf(stderr, "%s\n", crit[i]);
      fprintf(stderr,
              "\nSystem may become unusable.\n\n"
              "Use --force only if you know what you are doing.\n");
      exit(1);
    }
    if (ncrit > 0 && flags.force) {
      printf("Warning:\n\n");
      printf("Removing critical packages.\n\n");
      printf("Requested:\n\n");
      for (int i = 0; i < ncrit; i++)
        printf("%s\n", crit[i]);
      printf("\nSystem may become unusable.\n\n");
    }
  }

  /* ── 3. reverse-dep / recursive expand ──────────────────────────── */
  int nunused = 0;
  if (flags.recursive) {
    expand_unused_deps(pkgs, &npkgs, 256, pkg_storage);
    nunused = npkgs - nexplicit;
  } else if (!flags.force) {
    for (int i = 0; i < nexplicit; i++) {
      char breakers[64][LPM_NAME_MAX];
      int nb = collect_breakers(pkgs[i], pkgs, nexplicit, breakers, 64);
      if (nb > 0) {
        fprintf(stderr, "Error:\n\n");
        fprintf(stderr, "Removing %s breaks:\n\n", pkgs[i]);
        for (int b = 0; b < nb; b++)
          fprintf(stderr, "%s\n", breakers[b]);
        fprintf(stderr, "\nCannot continue.\n");
        exit(1);
      }
    }
  }

  /* ── 4. package list summary ────────────────────────────────────── */
  long total_freed = 0;
  for (int i = 0; i < npkgs; i++) {
    InstalledPkg ip; memset(&ip, 0, sizeof(ip));
    db_query(pkgs[i], &ip);
    total_freed += (long)ip.install_size;
  }

  if (flags.recursive && nunused > 0) {
    printf("Removing:\n\n");
    for (int i = 0; i < nexplicit; i++)
      printf("%s\n", pkgs[i]);
    printf("\nUnused dependencies:\n\n");
    for (int i = nexplicit; i < npkgs; i++)
      printf("%s\n", pkgs[i]);
  } else {
    printf("Removing packages:\n\n");
    for (int i = 0; i < npkgs; i++)
      printf("%s\n", pkgs[i]);
  }

  if (total_freed > 0) {
    char sfree[32];
    format_size(total_freed, sfree, sizeof(sfree));
    printf("\nFreed space: %s\n", sfree);
  }
  printf("\n");

  /* ── 5. confirm ─────────────────────────────────────────────────── */
  if (!flags.no_confirm) {
    if (!confirm("Proceed? [Y/n] "))
      return;
  }

  /* ── 6. execute removal ─────────────────────────────────────────── */
  int interrupted = 0;
  printf("\n");
  for (int i = 0; i < npkgs && !interrupted; i++) {
    if (g_cancel) { interrupted = 1; break; }

    const char *name = pkgs[i];
    printf("Removing %s...\n", name);
    fflush(stdout);
    lpm_log("Removing %s", name);
    journal_begin(name);

    int nfiles = db_files_remove(name);
    (void)nfiles;
    if (g_cancel) { interrupted = 1; break; }

    db_remove(name);

    {
      char cache[MAX_STR], rmcmd[MAX_STR];
      snprintf(cache,  sizeof(cache),  "%s/%s", LPM_BUILD_DIR, name);
      snprintf(rmcmd,  sizeof(rmcmd),  "rm -rf '%s'", cache);
      run(rmcmd);
    }

    journal_done(name);
    lpm_log("Removed %s", name);
    lpm_audit("remove: %s", name);
  }

  /* ── 7. post-remove ─────────────────────────────────────────────── */
  if (interrupted) {
    warn("remove aborted — system integrity is no longer guaranteed");
    return;
  }

  printf("\nDone.\n");
}


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
    if (!f) { printf("There is nothing to do.\n"); return; }
    char line[MAX_STR];
    while (fgets(line, sizeof(line), f) && ntargets < 256) {
      line[strcspn(line, "\n")] = '\0';
      if (!line[0]) continue;
      char *eq = strchr(line, '=');
      if (eq) *eq = '\0';
      targets[ntargets++] = strdup(line);
    }
    fclose(f);
  } else {
    for (int i = 0; i < argc && i < 256; i++)
      targets[ntargets++] = argv[i];
  }

  char *to_update[256];
  int nupdate = 0;
  char old_vers[256][LPM_VER_MAX + 16];
  char new_vers[256][LPM_VER_MAX + 16];

  for (int i = 0; i < ntargets; i++) {
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, targets[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0) { warn("No PKGBUILD for '%s'", targets[i]); continue; }

    if (lpm_config_is_ignored(&cfg, targets[i]))
      continue;

    Package pkg;
    pkgbuild_parse_fast(pbfile, &pkg);
    char pb_full[MAX_STR];
    snprintf(pb_full, sizeof(pb_full), "%s-%s", pkg.version, pkg.release);

    char *inst_ver = db_get_version(targets[i]);
    if (!inst_ver) {
      snprintf(old_vers[nupdate], sizeof(old_vers[0]), "unknown");
      snprintf(new_vers[nupdate], sizeof(new_vers[0]), "%s", pb_full);
      to_update[nupdate++] = targets[i];
    } else if (version_compare(inst_ver, pb_full) >= 0) {
      free(inst_ver);
    } else {
      snprintf(old_vers[nupdate], sizeof(old_vers[0]), "%s", inst_ver);
      snprintf(new_vers[nupdate], sizeof(new_vers[0]), "%s", pb_full);
      to_update[nupdate++] = targets[i];
      free(inst_ver);
    }
  }

  if (nupdate == 0) {
    printf("There is nothing to do.\n");
    return;
  }

  {
    int name_w = 0;
    for (int i = 0; i < nupdate; i++) {
      int len = (int)strlen(to_update[i]);
      if (len > name_w) name_w = len;
    }
    printf("Packages (%d)\n\n", nupdate);
    for (int i = 0; i < nupdate; i++)
      printf("%-*s  %s -> %s\n",
             name_w, to_update[i], old_vers[i], new_vers[i]);
    printf("\n");
  }

  if (!flags.no_confirm)
    if (!confirm("Proceed? [Y/n] "))
      return;

  printf("\n");
  for (int i = 0; i < nupdate; i++) {
    CHECK_CANCEL(update_done);
    printf("Upgrading %s...\n", to_update[i]);
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
  printf("\nDone.\n");
}

/* ── pkg_run_hook ──────────────────────────────────────────────────────
 * Runs the named hook function (pre_install/post_install — see
 * has_pre_install/has_post_install in include/lpm.h) from pkg's LPDF
 * file against root. Caller (tx_commit(), transaction.c) must check
 * has_pre_install/has_post_install before calling — this doesn't
 * re-check, so a missing function just fails the bash call.
 *
 * pkg_build()/pkg_run_check()/pkg_run_package() — the other three
 * "stubs for the Package API" that used to live here — were removed
 * instead: build()/check()/package() already run inline in
 * do_build_install() above, gated on has_check there; these Package-API
 * versions had zero callers anywhere in the codebase. pkg_run_hook() is
 * different: transaction.c really does call it. */
int pkg_run_hook(const char *hook, Package *pkg, const char *root) {
  if (!hook || !pkg || !root) return -1;

  char *converted = pkgbuild_to_bash(pkg->pkgbuild_path);
  const char *pbfile = converted ? converted : pkg->pkgbuild_path;

  char log[MAX_STR];
  pkg_log_path(pkg->name, log, sizeof(log));

  char cmd[MAX_CMD];
  snprintf(cmd, sizeof(cmd),
           "bash -c 'source \"%s\" && export ROOT=\"%s\" && %s'"
           " >> \"%s\" 2>&1",
           pbfile, root, hook, log);

  printf(":: Running %s()...\n", hook);
  int rc = run(cmd);

  if (converted) { unlink(converted); free(converted); }

  if (rc != 0) {
    fprintf(stderr, C_RED "error: " C_RESET
            "%s() failed for %s\n  Log: " C_CYAN "%s" C_RESET "\n",
            hook, pkg->name, log);
    lpm_log("%s() FAILED: %s", hook, pkg->name);
    return -1;
  }
  return 0;
}

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
int fetch_all_sources(char queue[][MAX_STR], int nqueue) {
  /* pass 1: count sources to size the FetchJob array */
  int total_srcs = 0;
  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) continue;
    for (int i = 0; i < pkg.nsources; i++)
      if (pkg.sources[i].url[0]) total_srcs++;
  }
  if (total_srcs == 0) return 0;

  FetchJob *jobs = calloc(total_srcs, sizeof(FetchJob));
  if (!jobs) return -1;
  int njobs = 0;

  /* pass 2: build FetchJob list, skipping already-cached files */
  for (int qi = 0; qi < nqueue; qi++) {
    if (g_cancel) { free(jobs); return -1; }

    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    Package pkg;
    if (pkgbuild_parse_fast(pbfile, &pkg) != 0) continue;
    if (db_is_installed(queue[qi])) continue;

    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.name);
    mkdir(ws, 0755);

    for (int i = 0; i < pkg.nsources; i++) {
      if (!pkg.sources[i].url[0]) continue;
      char *fname = strrchr(pkg.sources[i].url, '/');
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
        (void)system(cp_cmd);
        continue;
      }

      /* enqueue for download */
      FetchJob *j = &jobs[njobs++];
      snprintf(j->url,      LPM_URL_MAX,  "%s", pkg.sources[i].url);
      snprintf(j->dest,     LPM_PATH_MAX, "%s", dest);
      char label[LPM_NAME_MAX];
      snprintf(label, sizeof(label), "%s-%s", pkg.name, pkg.version);
      snprintf(j->filename, LPM_NAME_MAX, "%s", label);

      { CksumType _ct;
        const char *_hash = checksum_for_source(&pkg, i, &_ct);
        if (_hash) {
          strncpy(j->checksum, _hash, sizeof(j->checksum) - 1);
          j->cksum_type = _ct;
        } else {
          j->cksum_type = CKSUM_SKIP;
        }
      }
    }
  }

  int ret = (njobs > 0) ? dl_fetch_all(jobs, njobs) : 0;
  free(jobs);

  /* verify checksums after all downloads complete */
  if (ret == 0) {
    for (int qi = 0; qi < nqueue; qi++) {
      char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
      snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
      Package pkg;
      if (pkgbuild_parse_fast(pbfile, &pkg) != 0) continue;
      if (db_is_installed(queue[qi])) continue;

      char ws[MAX_STR];
      snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.name);

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

#pragma GCC diagnostic pop

/* ════════════════════════════════════════════════════════════════════════════
 * cmd_bootstrap — install Lotus Linux base system into a target directory
 *
 *  Usage:
 *    lpm bootstrap <target>               # install default base group
 *    lpm bootstrap <target> <pkg> [...]   # install specific packages
 *    lpm bootstrap /mnt/lotus             # → full base into /mnt/lotus
 *
 *  Equivalent to Arch's pacstrap:
 *    pacstrap /mnt base base-devel
 *    → lpm bootstrap /mnt/lotus
 *
 *  What it does:
 *    1. Create target directory structure (FHS)
 *    2. Init a fresh lpm DB inside target
 *    3. Sync repo databases
 *    4. Fetch + build all base packages
 *    5. Install each into <target>/ via tx_commit(tx, target)
 *    6. Configure: fstab hint, locale, hostname, resolv.conf stub
 *    7. Print chroot instructions
 *
 * ════════════════════════════════════════════════════════════════════════════ */

/* Default base package list — mirrors repo-lotus/base/ */
static const char *BOOTSTRAP_BASE[] = {
    /* toolchain */
    "musl", "gcc", "binutils",
    /* core */
    "busybox", "linux", "linux-headers",
    /* init */
    "dinit",
    /* package manager */
    "lpm",
    /* shell + utils */
    "bash", "grep", "sed", "awk", "make",
    "coreutils", "findutils", "diffutils",
    "tar", "gzip", "xz", "zstd", "bzip2",
    "file", "patch", "perl",
    /* fs + boot */
    "e2fsprogs", "util-linux", "limine",
    /* net */
    "iproute2", "dhcpcd", "wpa_supplicant", "openssh",
    /* dev */
    "pkgconf", "autoconf", "automake", "libtool",
    "meson", "ninja", "cmake", "git",
    /* lib */
    "zlib", "openssl", "curl", "wget",
    "libffi", "pcre2", "readline", "ncurses",
    "gdbm", "sqlite", "expat",
    /* man */
    "man-db", "man-pages",
    NULL
};

/* Create FHS directory skeleton inside target */
static void bootstrap_mkdirs(const char *target)
{
    static const char *dirs[] = {
        "bin", "boot", "dev", "etc", "etc/dinit.d",
        "home", "lib", "lib64",
        "mnt", "opt", "proc", "root", "run",
        "sbin", "srv", "sys", "tmp",
        "usr", "usr/bin", "usr/sbin", "usr/lib", "usr/lib64",
        "usr/include", "usr/share", "usr/share/doc",
        "usr/local", "usr/local/bin", "usr/local/lib",
        "var", "var/cache", "var/cache/lpm",
        "var/lib", "var/lib/lpm", "var/lib/lpm/db",
        "var/log", "var/log/lpm",
        "var/src", "usr/src/lpm",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        char p[LPM_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", target, dirs[i]);
        if (mkdir(p, 0755) != 0 && errno != EEXIST) {
            /* non-fatal — may already exist */
        }
    }
}

/* Write stub files needed for a minimal bootable system */
static void bootstrap_stubs(const char *target)
{
    char p[LPM_PATH_MAX]; FILE *f;

    /* /etc/lotus-release */
    snprintf(p, sizeof(p), "%s/etc/lotus-release", target);
    if ((f = fopen(p, "w"))) {
        fprintf(f, "Lotus Linux (Closed Beta)\n");
        fprintf(f, "Built with lpm bootstrap\n");
        fclose(f);
    }

    /* /etc/hostname */
    snprintf(p, sizeof(p), "%s/etc/hostname", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f, "lotus\n"); fclose(f);
    }

    /* /etc/hosts */
    snprintf(p, sizeof(p), "%s/etc/hosts", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f, "127.0.0.1   localhost\n"
                   "::1         localhost\n"
                   "127.0.1.1   lotus.localdomain lotus\n");
        fclose(f);
    }

    /* /etc/resolv.conf stub */
    snprintf(p, sizeof(p), "%s/etc/resolv.conf", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n");
        fclose(f);
    }

    /* /etc/passwd minimal */
    snprintf(p, sizeof(p), "%s/etc/passwd", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f, "root:x:0:0:root:/root:/bin/sh\n"
                   "nobody:x:65534:65534:nobody:/:/sbin/nologin\n");
        fclose(f);
    }

    /* /etc/group minimal */
    snprintf(p, sizeof(p), "%s/etc/group", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f, "root:x:0:\nnobody:x:65534:\n");
        fclose(f);
    }

    /* /etc/lpm/lpm.conf — copy from host if exists */
    snprintf(p, sizeof(p), "%s/etc/lpm", target);
    mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/etc/lpm/lpm.conf", target);
    if (access(p, F_OK) != 0) {
        char cmd[LPM_PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd),
                 "cp '%s' '%s' 2>/dev/null || true",
                 LPM_CONF_FILE, p);
        if (system(cmd)) { /* non-fatal */ }
    }

    /* /etc/dinit.d/boot — minimal dinit service */
    snprintf(p, sizeof(p), "%s/etc/dinit.d/boot", target);
    if (access(p, F_OK) != 0 && (f = fopen(p, "w"))) {
        fprintf(f,
            "# Lotus Linux boot service\n"
            "type = scripted\n"
            "start-script = /etc/dinit.d/scripts/boot.sh\n");
        fclose(f);
    }
}

/* Mount pseudo-filesystems for chroot builds (if needed) */
__attribute__((unused))
static void bootstrap_bind_mounts(const char *target, int do_mount)
{
    static const char *mnts[][2] = {
        { "proc",    "proc"    },
        { "sys",     "sys"     },
        { "dev",     "dev"     },
        { "dev/pts", "dev/pts" },
        { NULL, NULL }
    };
    for (int i = 0; mnts[i][0]; i++) {
        char cmd[LPM_PATH_MAX * 2];
        if (do_mount)
            snprintf(cmd, sizeof(cmd),
                     "mount --bind /%s '%s/%s' 2>/dev/null",
                     mnts[i][0], target, mnts[i][1]);
        else
            snprintf(cmd, sizeof(cmd),
                     "umount -l '%s/%s' 2>/dev/null",
                     target, mnts[i][1]);
        if (system(cmd)) { /* non-fatal */ }
    }
}

void cmd_bootstrap(int argc, char **argv)
{
    check_root();

    const char *target = NULL;
    const char **pkglist = NULL;
    int npkgs = 0;

    /* ── parse: lpm bootstrap -C <target> [package...] ──────────────── */
    int i = 0;
    if (argc >= 2 && !strcmp(argv[0], "-C")) {
        target = argv[1];
        i = 2;
    } else if (argc >= 1 && argv[0][0] != '-') {
        /* legacy: lpm bootstrap <target> [package...] */
        target = argv[0];
        i = 1;
    }

    if (!target) {
        die("No target specified.\nUsage: lpm bootstrap -C <target> [package...]");
    }

    if (argc > i) {
        /* user-specified packages */
        pkglist = (const char **)&argv[i];
        npkgs   = argc - i;
    } else {
        /* default: full base */
        for (npkgs = 0; BOOTSTRAP_BASE[npkgs]; npkgs++) {}
        pkglist = BOOTSTRAP_BASE;
    }

    /* ── header ──────────────────────────────────────────────────────── */
    printf("\n");
    printf(":: Lotus Linux Bootstrap\n");
    printf("   target : %s\n", target);
    printf("   packages: %d\n\n", npkgs);

    /* ── 1. create target structure ──────────────────────────────────── */
    printf(":: Creating target filesystem structure...\n");
    if (mkdir(target, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, C_RED "error: " C_RESET
                "cannot create target: %s: %s\n", target, strerror(errno));
        exit(1);
    }
    bootstrap_mkdirs(target);
    printf("   [ok] FHS skeleton created\n");

    /* ── 2. write stub config files ──────────────────────────────────── */
    bootstrap_stubs(target);
    printf("   [ok] base configs written\n");

    /* ── 3. sync repo databases ──────────────────────────────────────── */
    printf("\n:: Synchronizing repositories...\n");
    cmd_suy(0, NULL);   /* this syncs DBs and checks for updates */

    /* ── 4. fetch + build + install each package into target ─────────── */
    printf("\n:: Installing %d package(s) into %s\n\n", npkgs, target);

    LpmConfig cfg;
    lpm_config_load(LPM_CONF_FILE, &cfg);

    int ok = 0, fail = 0;
    time_t bs_start = time(NULL);

    for (int i = 0; i < npkgs; i++) {
        const char *name = pkglist[i];
        printf("[%d/%d] " C_BOLD "%s" C_RESET "\n", i + 1, npkgs, name);
        fflush(stdout);

        /* fetch PKGBUILD */
        char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
        snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s",
                 LPM_PKGBUILD_DIR, name);

        if (access(pbfile, F_OK) != 0) {
            if (fetch_pkgbuild(name) != 0) {
                printf(C_YELLOW "  [skip]" C_RESET
                       " pkgbuild not found: %s\n", name);
                fail++;
                continue;
            }
        }

        /* parse PKGBUILD */
        Package pkg; memset(&pkg, 0, sizeof(pkg));
        if (pkgbuild_parse_fast(pbfile, &pkg) != 0) {
            fprintf(stderr, C_YELLOW "  [warn]" C_RESET
                    " failed to parse pkgbuild for %s\n", name);
        }
        if (!pkg.name[0])
            strncpy(pkg.name, name, sizeof(pkg.name) - 1);

        /* check if binary package available */
        if (pkg.type == PKG_TYPE_BINARY) {
            /* try to pull .lpkg from repo */
            char arch[32] = "x86_64";
            FILE *fp = popen("uname -m 2>/dev/null", "r");
            if (fp) {
                if (fgets(arch, sizeof(arch), fp)) {
                    char *nl = strchr(arch, '\n'); if (nl) *nl = '\0';
                }
                pclose(fp);
            }
            char fname[LPM_PATH_MAX];
            snprintf(fname, sizeof(fname),
                     "%s-%s-%s-%s.lpkg",
                     name,
                     pkg.version[0] ? pkg.version : "0",
                     pkg.release[0] ? pkg.release : "1",
                     arch);
            char dest[LPM_PATH_MAX + 256];
            snprintf(dest, sizeof(dest),
                     "/var/cache/lpm/packages/%s", fname);

            if (access(dest, F_OK) == 0) {
                /* extract lpkg into target */
                char cmd[LPM_PATH_MAX * 2];
                snprintf(cmd, sizeof(cmd),
                         "tar -xf '%s' -C '%s' 2>/dev/null", dest, target);
                if (system(cmd) == 0) {
                    printf(C_GREEN "  ==> Installed " C_RESET
                           C_BOLD "%s" C_RESET " [binary]\n", name);
                    ok++;
                    continue;
                }
            }
        }

        /* source build — redirect pkgdir into target */
        char ws[MAX_STR];
        snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.name);
        util_mkdirp(ws, 0755);

        /* Override pkgdir to install into target */
        char pkgdir_target[LPM_PATH_MAX];
        snprintf(pkgdir_target, sizeof(pkgdir_target),
                 "%s/%s-pkgdir", LPM_BUILD_DIR, pkg.name);
        util_mkdirp(pkgdir_target, 0755);

        LpmFlags bflags; memset(&bflags, 0, sizeof(bflags));

        /* fetch sources */
        char _bqueue[1][MAX_STR];
        strncpy(_bqueue[0], pkg.name, MAX_STR - 1);
        if (fetch_all_sources(_bqueue, 1) != 0) {
            printf(C_YELLOW "  [skip]" C_RESET
                   " source download failed: %s\n", name);
            fail++;
            continue;
        }

        /* srcdir detection */
        char srcdir[MAX_STR * 2];
        snprintf(srcdir, sizeof(srcdir), "%s/%s-%s",
                 ws, pkg.name, pkg.version);
        {
            struct stat _sd;
            if (stat(srcdir, &_sd) != 0 || !S_ISDIR(_sd.st_mode))
                snprintf(srcdir, sizeof(srcdir), "%s", ws);
        }

        /* run build() + package() via temp script, override pkgdir */
        char *converted = pkgbuild_to_bash(pbfile);
        const char *pb = converted ? converted : pbfile;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        char build_cmd[MAX_CMD];
        snprintf(build_cmd, sizeof(build_cmd),
            "bash -c 'source \"%s\" && cd \"%s\""
            " && export srcdir=\"%s\" startdir=\"%s\""
            " && export pkgdir=\"%s\""
            " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\""
            " LDFLAGS=\"%s\" MAKEFLAGS=\"-j$(nproc)\""
            " && build"
            "' >> /var/log/lpm/%s-bootstrap.log 2>&1",
            pb, srcdir, srcdir, ws, pkgdir_target,
            "-O2 -pipe", "-O2 -pipe", "-Wl,-O1",
            pkg.name);

        if (run(build_cmd) != 0) {
            printf(C_RED "  [fail]" C_RESET " build failed: %s\n", name);
            if (converted) { unlink(converted); free(converted); }
            fail++;
            continue;
        }

        char inst_cmd[MAX_CMD];
        snprintf(inst_cmd, sizeof(inst_cmd),
            "bash -c 'source \"%s\" && cd \"%s\""
            " && export srcdir=\"%s\" pkgdir=\"%s\""
            " && package"
            "' >> /var/log/lpm/%s-bootstrap.log 2>&1",
            pb, srcdir, srcdir, pkgdir_target, pkg.name);
#pragma GCC diagnostic pop

        if (run(inst_cmd) != 0) {
            printf(C_RED "  [fail]" C_RESET " package() failed: %s\n", name);
            if (converted) { unlink(converted); free(converted); }
            fail++;
            continue;
        }
        if (converted) { unlink(converted); free(converted); }

        /* merge pkgdir into target */
        snprintf(pkg.pkg_dir, sizeof(pkg.pkg_dir), "%s", pkgdir_target);

        Transaction *tx = tx_new();
        if (!tx) { fail++; continue; }
        tx_add_install(tx, &pkg);

        if (tx_commit(tx, target) == 0) {
            printf(C_GREEN "  ==> Installed " C_RESET
                   C_BOLD "%s %s-%s" C_RESET " → %s\n",
                   pkg.name, pkg.version, pkg.release, target);
            ok++;
        } else {
            printf(C_RED "  [fail]" C_RESET
                   " merge failed: %s\n", name);
            fail++;
        }
        tx_free(tx);

        /* cleanup pkgdir */
        char rmcmd[LPM_PATH_MAX + 16];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", pkgdir_target);
        if (system(rmcmd)) { /* non-fatal */ }
    }

    /* ── 5. post-bootstrap summary ───────────────────────────────────── */
    long elapsed = (long)(time(NULL) - bs_start);
    printf("\n");
    printf("── Bootstrap complete " C_GRAY "(%ldm%lds)" C_RESET " ─────────────────────\n",
           elapsed / 60, elapsed % 60);
    printf("  installed : " C_GREEN "%d" C_RESET "\n", ok);
    if (fail)
        printf("  failed    : " C_RED "%d" C_RESET "\n", fail);
    printf("  target    : %s\n\n", target);

    /* ── 6. chroot instructions ──────────────────────────────────────── */
    printf("Next steps:\n\n");
    printf("  # mount pseudo-filesystems\n");
    printf("  mount --bind /proc  %s/proc\n",    target);
    printf("  mount --bind /sys   %s/sys\n",     target);
    printf("  mount --bind /dev   %s/dev\n",     target);
    printf("  mount --bind /dev/pts %s/dev/pts\n\n", target);
    printf("  # enter chroot\n");
    printf("  chroot %s /bin/bash\n\n", target);
    printf("  # inside chroot — configure your system\n");
    printf("  echo \"lotus\" > /etc/hostname\n");
    printf("  passwd root\n");
    printf("  dinit-setup  # if available\n\n");
    printf("  # install bootloader (BIOS/EFI)\n");
    printf("  limine bios-install /dev/sdX\n\n");
}
