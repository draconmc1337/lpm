#include "lpm.h"
#include <stdarg.h>

static int run(const char *cmd) { return system(cmd); }

/* forward declaration */
static int fetch_all_sources(char queue[][MAX_STR], int nqueue);

/* ── pkg_log_path ────────────────────────────────────────────────────── */
void pkg_log_path(const char *pkgname, char *out, size_t outsz) {
  snprintf(out, outsz, "%s/%s.log", LPM_LOG_DIR, pkgname);
}

/* ── LpmFlags parser — only --force and --no-confirm remain ──────────── */
int lpm_parse_flags(int argc, char **argv, LpmFlags *f, char **pkgs,
                    int maxpkgs) {
  memset(f, 0, sizeof(*f));
  int n = 0;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--force"))
      f->force = 1;
    else if (!strcmp(argv[i], "--no-confirm"))
      f->no_confirm = 1;
    else if (n < maxpkgs)
      pkgs[n++] = argv[i];
  }
  return n;
}

/* ── fetch_url: retry logic ──────────────────────────────────────────── */
#define FETCH_RETRIES 3
#define FETCH_DELAY 1

static int fetch_url(const char *url, const char *dest) {
  char part[MAX_STR];
  snprintf(part, sizeof(part), "%s.part", dest);

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
    remove(part);
    if (attempt > 1) {
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

    if (rc == 0 && have_part && part_st.st_size >= (200 * 1024)) {
      if (rename(part, dest) == 0) {
        lpm_log("FETCH OK: %s (attempt %d)", url, attempt);
        return 0;
      }
      remove(part);
      return -1;
    }

    if (host[0]) {
      char dns_cmd[MAX_STR];
      snprintf(dns_cmd, sizeof(dns_cmd), "getent hosts '%s' >/dev/null 2>&1",
               host);
      if (run(dns_cmd) != 0) {
        fprintf(stderr,
                C_RED "error: " C_RESET
                      "network unavailable (DNS failed for %s)\n",
                host);
        remove(part);
        return -1;
      }
    }

    if (have_part && part_st.st_size > 0 && part_st.st_size < (200 * 1024))
      fprintf(stderr,
              C_YELLOW "warning: " C_RESET
              "attempt %d: response too small (%ld bytes) — likely 404\n",
              attempt, (long)part_st.st_size);
    else
      fprintf(stderr,
              C_YELLOW "warning: " C_RESET "attempt %d failed (rc=%d)\n",
              attempt, rc);
    remove(part);
  }

  fprintf(stderr,
          C_RED "error: " C_RESET
                "network timeout fetching %s\n  Failed after %d attempts.\n",
          url, FETCH_RETRIES);
  return -1;
}

/* ── prepare_workspace ───────────────────────────────────────────────── */
static int prepare_workspace(Pkg *pkg) {
  char ws[MAX_STR];
  snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg->pkgname);
  mkdir(ws, 0755);

  for (int i = 0; i < pkg->nsources; i++) {
    if (!pkg->source[i][0])
      continue;
    char *fname = strrchr(pkg->source[i], '/');
    if (!fname)
      continue;
    fname++;

    char dest[MAX_STR];
    snprintf(dest, sizeof(dest), "%s/%s", ws, fname);

    struct stat st;
    if (stat(dest, &st) == 0) {
      if (st.st_size < (200 * 1024)) {
        fprintf(stderr,
                C_YELLOW "warning: " C_RESET
                         "%s looks partial (%ld bytes), re-downloading...\n",
                fname, (long)st.st_size);
        remove(dest);
      } else {
        int is_tar = (strstr(fname, ".tar") != NULL);
        if (is_tar) {
          char chk[MAX_STR];
          snprintf(chk, sizeof(chk), "tar -tf '%s' &>/dev/null", dest);
          if (run(chk) != 0) {
            fprintf(stderr,
                    C_YELLOW "warning: " C_RESET
                             "%s is corrupt, re-downloading...\n",
                    fname);
            remove(dest);
          } else {
            continue;
          }
        } else {
          continue;
        }
      }
    }

    char local_src[MAX_STR];
    snprintf(local_src, sizeof(local_src), "/sources/%s", fname);
    struct stat local_st;
    if (stat(local_src, &local_st) == 0 && local_st.st_size > 0) {
      printf(C_CYAN "  ->" C_RESET " Using local source: %s\n", local_src);
      char cp_cmd[MAX_STR];
      snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'", local_src, dest);
      if (run(cp_cmd) != 0) {
        fprintf(stderr,
                C_RED "error: " C_RESET "Failed to copy from /sources/%s\n",
                fname);
        return -1;
      }
      continue;
    }

    printf(C_CYAN "  ->" C_RESET " Fetching %s\n", fname);
    if (fetch_url(pkg->source[i], dest) != 0)
      return -1;
  }
  return 0;
}

/* ── verify_sources ──────────────────────────────────────────────────── */
static int verify_sources(Pkg *pkg, const char *ws) {
  int ok = 1;
  for (int i = 0; i < pkg->nsources; i++) {
    const char *expected = NULL, *algo = NULL, *tool = NULL;

    if (pkg->sha256sums[i][0] && strcmp(pkg->sha256sums[i], "SKIP") != 0) {
      expected = pkg->sha256sums[i];
      algo = "sha256";
      tool = "sha256sum";
    } else if (pkg->md5sums[i][0] && strcmp(pkg->md5sums[i], "SKIP") != 0) {
      expected = pkg->md5sums[i];
      algo = "md5";
      tool = "md5sum";
    } else
      continue;

    char *fname = strrchr(pkg->source[i], '/');
    if (!fname)
      continue;
    fname++;

    char filepath[MAX_STR];
    snprintf(filepath, sizeof(filepath), "%s/%s", ws, fname);

    char cmd[MAX_STR];
    snprintf(cmd, sizeof(cmd), "%s '%s' 2>/dev/null | cut -d' ' -f1", tool,
             filepath);
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
                    "\n  got:      " C_RED "%s" C_RESET "\n",
              algo, fname, expected, actual);
      ok = 0;
    } else {
      printf(C_GREEN "  ok" C_RESET " [%s] %s\n", algo, fname);
    }
  }
  return ok;
}

/* ── shared build+install core ───────────────────────────────────────── */
static void do_build_install(Pkg *pkg, const char *pbfile, LpmConfig *cfg,
                             int qi, int nqueue) {
  char ws[MAX_STR];
  snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg->pkgname);
  mkdir(ws, 0755);

  char pkg_log[MAX_STR];
  pkg_log_path(pkg->pkgname, pkg_log, sizeof(pkg_log));

  printf(C_BOLD "[%d/%d] Building %s %s-%s" C_RESET "\n", qi + 1, nqueue,
         pkg->pkgname, pkg->pkgver, pkg->pkgrel);
  lpm_log("Building %s %s-%s", pkg->pkgname, pkg->pkgver, pkg->pkgrel);

  char build_cmd[MAX_CMD];
  snprintf(build_cmd, sizeof(build_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " && build' > \"%s\" 2>&1",
           pbfile, ws, cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx, pkg_log);

  if (run(build_cmd) != 0) {
    fprintf(stderr,
            C_RED "error: " C_RESET "Build failed: %s\n"
                  "  Phase: build()\n"
                  "  Log:   " C_CYAN "%s" C_RESET "\n",
            pkg->pkgname, pkg_log);
    lpm_log("Build FAILED: %s", pkg->pkgname);
    exit(1);
  }

  /* check() — controlled entirely by lpm.conf */
  if (pkg->has_check && cfg->run_check) {
    printf(C_CYAN "::" C_RESET " Running check()...\n");
    char check_cmd[MAX_CMD];
    snprintf(check_cmd, sizeof(check_cmd),
             "bash -c 'source \"%s\" && cd \"%s\" && check'"
             " >> \"%s\" 2>&1",
             pbfile, ws, pkg_log);
    int rc = run(check_cmd);
    char tail_cmd[MAX_STR];
    snprintf(tail_cmd, sizeof(tail_cmd), "tail -40 '%s'", pkg_log);
    run(tail_cmd);
    if (rc != 0) {
      if (cfg->strict_build) {
        fprintf(stderr,
                C_RED "error: " C_RESET
                      "check() failed (rc=%d) — blocked by STRICT_BUILD\n"
                      "  See log: " C_CYAN "%s" C_RESET "\n",
                rc, pkg_log);
        exit(1);
      }
      printf(C_YELLOW "warning: " C_RESET
                      "check() failed (rc=%d) — may be safe to ignore\n"
                      "  Log: " C_CYAN "%s" C_RESET "\n",
             rc, pkg_log);
      if (!cfg->default_yes)
        if (!confirm("Continue to install anyway? [" C_GREEN "Yes" C_RESET
                     "/" C_RED "No" C_RESET "] "))
          exit(1);
    } else {
      printf(C_GREEN "  check() passed" C_RESET "\n");
    }
  }

  /* package() → pkgdir */
  char pkgdir[MAX_STR];
  snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", ws);
  char mk_cmd[MAX_STR];
  snprintf(mk_cmd, sizeof(mk_cmd), "rm -rf '%s' && mkdir -p '%s'",
           pkgdir, pkgdir);
  run(mk_cmd);

  printf(C_BOLD "==> Staging %s" C_RESET "\n", pkg->pkgname);
  lpm_log("Staging %s", pkg->pkgname);

  char inst_cmd[MAX_CMD];
  snprintf(inst_cmd, sizeof(inst_cmd),
           "bash -c 'source \"%s\" && cd \"%s\""
           " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
           " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
           " pkgdir=\"%s\""
           " && package' >> \"%s\" 2>&1",
           pbfile, ws, cfg->cflags, cfg->cxxflags, cfg->ldflags,
           cfg->makeflags, cfg->cc, cfg->cxx, pkgdir, pkg_log);

  if (run(inst_cmd) != 0) {
    fprintf(stderr,
            C_RED "error: " C_RESET "Install failed for %s\n"
                  "  Phase: package()\n"
                  "  See log: " C_CYAN "%s" C_RESET "\n",
            pkg->pkgname, pkg_log);
    lpm_log("Install FAILED: %s", pkg->pkgname);
    exit(1);
  }

  printf(C_CYAN "  ->" C_RESET " Merging into /...\n");
  char merge_cmd[MAX_STR];
  snprintf(merge_cmd, sizeof(merge_cmd),
           "cp -a --remove-destination '%s'/. /", pkgdir);
  if (run(merge_cmd) != 0) {
    fprintf(stderr, C_RED "error: " C_RESET "Merge failed for %s\n",
            pkg->pkgname);
    exit(1);
  }

  db_files_save(pkg->pkgname, pkgdir);
  db_add(pkg->pkgname, pkg->pkgver, pkg->pkgrel);
  lpm_audit("install: %s %s-%s", pkg->pkgname, pkg->pkgver, pkg->pkgrel);

  printf(C_GREEN "==> Installed %s %s-%s" C_RESET "\n",
         pkg->pkgname, pkg->pkgver, pkg->pkgrel);
  lpm_log("Installed %s %s-%s", pkg->pkgname, pkg->pkgver, pkg->pkgrel);
}

/* ── cmd_fetch ───────────────────────────────────────────────────────── */
#define REPO_BASE \
  "https://raw.githubusercontent.com/draconmc1337/lotus-repository/main"
static const char *FOLDERS[] = {"base", "extra", "lotus"};
#define NFOLDERS 3

static int fetch_pkgbuild(const char *name) {
  char dest[MAX_STR];
  snprintf(dest, sizeof(dest), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, name);
  printf(C_CYAN "::" C_RESET " Fetching pkgbuild_%s...\n", name);
  for (int f = 0; f < NFOLDERS; f++) {
    char url[MAX_STR];
    snprintf(url, sizeof(url), "%s/%s/pkgbuild_%s", REPO_BASE, FOLDERS[f],
             name);
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
             "wget -q -O '%s' '%s' 2>/dev/null"
             " || curl -sfL -o '%s' '%s' 2>/dev/null",
             dest, url, dest, url);
    run(cmd);
    struct stat st;
    if (stat(dest, &st) == 0 && st.st_size > 32) {
      printf(C_GREEN "  ->" C_RESET " Found in " C_CYAN "[%s]" C_RESET "\n",
             FOLDERS[f]);
      lpm_log("Fetched pkgbuild_%s from %s/%s", name, REPO_BASE, FOLDERS[f]);
      return 0;
    }
    remove(dest);
  }
  return -1;
}

void cmd_fetch(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm -Sy <package>");
  for (int i = 0; i < argc; i++) {
    if (fetch_pkgbuild(argv[i]) != 0)
      fprintf(stderr,
              C_RED "error: " C_RESET
                    "pkgbuild_%s not found in base/, extra/, or lotus/\n",
              argv[i]);
  }
  printf(C_CYAN "::" C_RESET " PKGBUILDs saved to " C_BOLD "%s" C_RESET "\n"
                "   Review then run: lpm -bi <package>\n",
         LPM_PKGBUILD_DIR);
}

/* ── cmd_local (-bi) ─────────────────────────────────────────────────── */
void cmd_local(int argc, char **argv) {
  check_root();
  init_dirs();

  /* load config FIRST — drives all behaviour */
  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -bi <package>");

  for (int i = 0; i < npkgs; i++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             pkgs[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0)
      die("PKGBUILD not found locally for '%s'\n"
          "    Expected: %s\n"
          "    Use 'lpm -S' to fetch from network.",
          pkgs[i], pbfile);
  }

  /* build queue with toposort */
  char queue[256][MAX_STR];
  int nqueue = 0;
  for (int i = 0; i < npkgs; i++) {
    char subq[256][MAX_STR];
    int nsub = dep_resolve_queue(pkgs[i], subq, 256);
    for (int j = 0; j < nsub && nqueue < 256; j++) {
      int dup = 0;
      for (int k = 0; k < nqueue; k++)
        if (!strcmp(queue[k], subq[j])) { dup = 1; break; }
      if (!dup)
        strncpy(queue[nqueue++], subq[j], MAX_STR - 1);
    }
    int dup = 0;
    for (int k = 0; k < nqueue; k++)
      if (!strcmp(queue[k], pkgs[i])) { dup = 1; break; }
    if (!dup)
      strncpy(queue[nqueue++], pkgs[i], MAX_STR - 1);
  }

  cmd_deptree(npkgs, pkgs);

  if (nqueue > 0) {
    printf(C_CYAN "::" C_RESET " Will build " C_BOLD "%d" C_RESET
                  " package(s) in order:\n", nqueue);
    for (int i = 0; i < nqueue; i++)
      printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
    printf("\n");
  }

  if (!cfg.default_yes)
    if (!confirm("\nBuild these packages? [" C_GREEN "Yes" C_RESET
                 "/" C_RED "No" C_RESET "] ")) {
      printf("Aborted.\n");
      exit(0);
    }

  /* ── Phase 1: fetch ALL sources first (parallel) ── */
  if (fetch_all_sources(queue, nqueue) != 0)
    die("Source fetch failed — aborting build.");

  /* ── Phase 2: build + install in topo order ── */
  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) {
      warn("No local PKGBUILD for '%s', skipping", queue[qi]);
      continue;
    }
    if (db_is_installed(queue[qi])) {
      printf(C_CYAN "  ->" C_RESET " %s already installed, skipping\n",
             queue[qi]);
      continue;
    }
    do_build_install(&pkg, pbfile, &cfg, qi, nqueue);
  }
}

/* ── cmd_sync (-S) ───────────────────────────────────────────────────── */
void cmd_sync(int argc, char **argv) {
  check_root();
  init_dirs();

  /* load config FIRST */
  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  LpmFlags flags;
  char *pkgs[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -S <package>");

  /* fetch PKGBUILDs */
  for (int i = 0; i < npkgs; i++)
    if (fetch_pkgbuild(pkgs[i]) != 0)
      die("pkgbuild_%s not found in base/, extra/, or lotus/\n"
          "    Check the package name or push PKGBUILD to the repo.",
          pkgs[i]);

  /* build full dep queue */
  char queue[256][MAX_STR];
  int nqueue = 0;
  for (int i = 0; i < npkgs; i++) {
    char subq[256][MAX_STR];
    int nsub = dep_resolve_queue(pkgs[i], subq, 256);
    for (int j = 0; j < nsub && nqueue < 256; j++) {
      int dup = 0;
      for (int k = 0; k < nqueue; k++)
        if (!strcmp(queue[k], subq[j])) { dup = 1; break; }
      if (!dup)
        strncpy(queue[nqueue++], subq[j], MAX_STR - 1);
    }
    int dup = 0;
    for (int k = 0; k < nqueue; k++)
      if (!strcmp(queue[k], pkgs[i])) { dup = 1; break; }
    if (!dup)
      strncpy(queue[nqueue++], pkgs[i], MAX_STR - 1);
  }

  cmd_deptree(npkgs, pkgs);

  if (nqueue > 0) {
    printf(C_CYAN "::" C_RESET " Will build " C_BOLD "%d" C_RESET
                  " package(s) in order:\n", nqueue);
    for (int i = 0; i < nqueue; i++)
      printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
    printf("\n");
  }

  if (!cfg.default_yes)
    if (!confirm("\nBuild these packages? [" C_GREEN "Yes" C_RESET
                 "/" C_RED "No" C_RESET "] ")) {
      printf("Aborted.\n");
      exit(0);
    }

  /* fetch missing dep PKGBUILDs */
  for (int qi = 0; qi < nqueue; qi++) {
    if (db_is_installed(queue[qi]))
      continue;
    char pbf[MAX_STR];
    snprintf(pbf, sizeof(pbf), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, queue[qi]);
    struct stat st;
    if (stat(pbf, &st) != 0) {
      printf(C_CYAN "  ->" C_RESET " Fetching pkgbuild_%s...\n", queue[qi]);
      fetch_pkgbuild(queue[qi]);
    }
  }

  /* ── Phase 1: fetch ALL sources first (parallel) ── */
  if (fetch_all_sources(queue, nqueue) != 0)
    die("Source fetch failed — aborting build.");

  /* ── Phase 2: build + install in topo order ── */
  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) {
      warn("No PKGBUILD for '%s', skipping", queue[qi]);
      continue;
    }
    if (db_is_installed(queue[qi])) {
      printf(C_CYAN "  ->" C_RESET " %s already installed, skipping\n",
             queue[qi]);
      continue;
    }
    do_build_install(&pkg, pbfile, &cfg, qi, nqueue);
  }
}

/* ── cmd_check (-c) ──────────────────────────────────────────────────── */
void cmd_check(int argc, char **argv) {
  check_root();
  init_dirs();
  if (argc == 0)
    die("No package specified.\nUsage: lpm -c <package>");

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  for (int i = 0; i < argc; i++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             argv[i]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0)
      die("PKGBUILD not found for '%s'", argv[i]);

    char ws[MAX_STR];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);

    char built[MAX_STR];
    snprintf(built, sizeof(built), "%s/.built", ws);
    struct stat st;
    if (stat(built, &st) != 0)
      die("%s has not been built yet. Run 'lpm -bi %s' first.",
          argv[i], argv[i]);

    if (!pkg.has_check) {
      warn("No check() in pkgbuild_%s — skipping", argv[i]);
      continue;
    }

    if (!cfg.default_yes) {
      char prompt[MAX_STR];
      snprintf(prompt, sizeof(prompt),
               "Run test suite for " C_BOLD "%s" C_RESET "? [Y/N] ", argv[i]);
      if (!confirm(prompt)) {
        printf("Skipped.\n");
        continue;
      }
    }

    char pkg_log[MAX_STR];
    pkg_log_path(pkg.pkgname, pkg_log, sizeof(pkg_log));

    printf(C_CYAN "::" C_RESET " Running check() for " C_BOLD "%s" C_RESET
                  "...\n", argv[i]);
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
      printf(C_RED "check() failed (rc=%d)" C_RESET "  Log: " C_CYAN
                   "%s" C_RESET "\n", rc, pkg_log);
      lpm_log("check() failed for %s (rc=%d)", argv[i], rc);
      if (!cfg.default_yes)
        if (!confirm("Failures detected. Continue anyway? [y/N] "))
          exit(1);
    }
  }
}

/* ── cmd_remove (-r) ─────────────────────────────────────────────────── */
void cmd_remove(int argc, char **argv) {
  check_root();
  init_dirs();

  /* load config FIRST */
  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  char *pkgs[64];
  int npkgs = 0;
  int force = 0, no_confirm = 0;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--force"))
      force = 1;
    else if (!strcmp(argv[i], "--no-confirm"))
      no_confirm = 1;
    else
      pkgs[npkgs++] = argv[i];
  }
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -r <package> [--force] "
        "[--no-confirm]");

  if (force || no_confirm) {
    char pkglist[512] = "";
    for (int i = 0; i < npkgs; i++) {
      if (i)
        strncat(pkglist, " ", sizeof(pkglist) - strlen(pkglist) - 1);
      strncat(pkglist, pkgs[i], sizeof(pkglist) - strlen(pkglist) - 1);
    }
    lpm_audit("cmd_remove flags: force=%d no_confirm=%d packages: %s",
              force, no_confirm, pkglist);
  }

  /* reverse dep check */
  int blocked = 0;
  for (int i = 0; i < npkgs; i++) {
    char *rdeps = reverse_deps(pkgs[i]);
    if (rdeps && rdeps[0]) {
      fprintf(stderr,
              C_RED "error: " C_RESET C_BOLD "%s" C_RESET
                    " is required by: " C_YELLOW "%s" C_RESET "\n",
              pkgs[i], rdeps);
      blocked = 1;
    }
  }
  if (blocked && !force) {
    printf("Remove dependents first, or use " C_BOLD "-r --force" C_RESET
           ".\n");
    exit(1);
  }
  if (blocked && force)
    warn("--force: ignoring reverse dependencies.");

  /* ── verify all installed FIRST — bail before asking anything ── */
  int any_missing = 0;
  for (int i = 0; i < npkgs; i++) {
    if (!db_is_installed(pkgs[i])) {
      fprintf(stderr, C_RED "error: " C_RESET "not installed: %s\n", pkgs[i]);
      any_missing = 1;
    }
  }
  if (any_missing) exit(1);

  /* ── collect critical packages, one combined confirm for all ── */
  char crit_upper[512] = "";
  int  ncrit = 0;
  for (int i = 0; i < npkgs; i++) {
    if (!lpm_config_is_critical(&cfg, pkgs[i])) continue;
    if (!force) {
      fprintf(stderr,
              C_RED "error: " C_RESET
                    "'%s' is protected (CriticalPkg in lpm.conf)\n"
                    "Use " C_BOLD "--force" C_RESET " to override.\n",
              pkgs[i]);
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
    fprintf(stderr,
            C_RED C_BOLD "\nERROR: %s %s A CRITICAL PACKAGE!\n" C_RESET,
            crit_upper, ncrit > 1 ? "ARE" : "IS");
    if (!no_confirm && !cfg.default_yes) {
      if (!confirm_word("Type " C_BOLD "YES" C_RESET " to continue:\n> ",
                        "YES")) { printf("Aborted.\n"); exit(0); }
      if (!confirm_word(C_BOLD "FINAL CONFIRMATION (type YES):\n> " C_RESET,
                        "YES")) { printf("Aborted.\n"); exit(0); }
      if (!confirm_word("Type " C_BOLD "DELETE" C_RESET " to proceed:\n> ",
                        "DELETE")) { printf("Aborted.\n"); exit(0); }
    }
  }

  if (!no_confirm && !cfg.default_yes) {
    printf("Packages to remove (" C_BOLD "%d" C_RESET "):\n", npkgs);
    for (int i = 0; i < npkgs; i++)
      printf("  %s\n", pkgs[i]);
    printf("\n");
    if (!confirm("Remove? [" C_GREEN "Yes" C_RESET "/" C_RED "No" C_RESET
                 "] ")) {
      printf("Aborted.\n");
      exit(0);
    }
  }

  for (int i = 0; i < npkgs; i++) {
    printf(C_CYAN "::" C_RESET " Removing " C_BOLD "%s" C_RESET "...",
           pkgs[i]);
    fflush(stdout);
    lpm_log("Removing %s", pkgs[i]);

    int nfiles = db_files_remove(pkgs[i]);
    if (nfiles < 0)
      warn("no files.list for '%s' (pre-Hotfix2 install) — only removing DB",
           pkgs[i]);
    else
      printf(" (%d file(s))", nfiles);

    db_remove(pkgs[i]);

    char cache[MAX_STR], rm_cmd[MAX_STR];
    snprintf(cache, sizeof(cache), "%s/%s", LPM_BUILD_DIR, pkgs[i]);
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
    run(rm_cmd);

    printf(" " C_GREEN "done" C_RESET "\n");
    lpm_log("Removed %s (files=%d)", pkgs[i], nfiles);
    lpm_audit("remove: %s", pkgs[i]);
  }
}

/* ── cmd_update (-u) ─────────────────────────────────────────────────── */
void cmd_update(int argc, char **argv) {
  check_root();
  init_dirs();

  /* load config FIRST */
  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  char *targets[256];
  int ntargets = 0;

  if (argc == 0) {
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
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             targets[i]);
    struct stat st;
    if (stat(pbfile, &st) != 0) {
      warn("No PKGBUILD for '%s', skipping", targets[i]);
      continue;
    }
    if (lpm_config_is_ignored(&cfg, targets[i])) {
      printf("  " C_BOLD "%-24s" C_RESET "  " C_CYAN "ignored" C_RESET
             " (IgnorePkg)\n", targets[i]);
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
    } else if (!strcmp(inst_ver, pb_full)) {
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
    printf(C_CYAN "::" C_RESET " " C_GREEN "All packages are up to date."
           C_RESET "\n");
    return;
  }

  printf("Packages to update (" C_BOLD "%d" C_RESET "):\n", nupdate);
  for (int i = 0; i < nupdate; i++)
    printf("  %s\n", to_update[i]);
  printf("\n");

  if (!cfg.default_yes)
    if (!confirm("Rebuild and reinstall? [y/N] ")) {
      printf("Aborted.\n");
      exit(0);
    }

  for (int i = 0; i < nupdate; i++) {
    printf("\n" C_BOLD "==> Updating %s" C_RESET "\n", to_update[i]);
    char cache[MAX_STR], rm_cmd[MAX_STR];
    snprintf(cache, sizeof(cache), "%s/%s", LPM_BUILD_DIR, to_update[i]);
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", cache);
    run(rm_cmd);
    char *pair[1] = {to_update[i]};
    cmd_sync(1, pair);
    lpm_log("Updated %s", to_update[i]);
  }
  printf("\n" C_CYAN "::" C_RESET " " C_GREEN "Update complete." C_RESET "\n");
}

/* ── stubs for new Package API (used by transaction.c) ──────────────── */
int pkg_build(Package *pkg, const LpmConfig *cfg) {
  (void)pkg; (void)cfg; return 0;
}
int pkg_run_check(Package *pkg) { (void)pkg; return 0; }
int pkg_run_package(Package *pkg) { (void)pkg; return 0; }
int pkg_run_hook(const char *h, Package *pkg) { (void)h; (void)pkg; return 0; }

/* ══════════════════════════════════════════════════════════════════════
 * fetch_all_sources: build FetchJob array from name queue, run parallel dl
 * ══════════════════════════════════════════════════════════════════════ */
static int fetch_all_sources(char queue[][MAX_STR], int nqueue) {
  int total_srcs = 0;
  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             queue[qi]);
    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0) continue;
    for (int i = 0; i < pkg.nsources; i++)
      if (pkg.source[i][0]) total_srcs++;
  }

  if (total_srcs == 0) return 0;

  FetchJob *jobs = calloc(total_srcs, sizeof(FetchJob));
  if (!jobs) return -1;
  int njobs = 0;

  for (int qi = 0; qi < nqueue; qi++) {
    char pbfile[MAX_STR];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR,
             queue[qi]);
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
      if (stat(dest, &st) == 0 && st.st_size >= (200 * 1024)) {
        if (strstr(fname, ".tar")) {
          char chk[MAX_STR];
          snprintf(chk, sizeof(chk), "tar -tf '%s' &>/dev/null", dest);
          if (system(chk) == 0) continue;
        } else continue;
      }

      char local_src[MAX_STR];
      snprintf(local_src, sizeof(local_src), "/sources/%s", fname);
      if (stat(local_src, &st) == 0 && st.st_size > 0) {
        char cp_cmd[MAX_STR];
        snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'", local_src, dest);
        system(cp_cmd);
        continue;
      }

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
  return ret;
}
