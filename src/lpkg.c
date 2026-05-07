#include "lpm.h"
#include <time.h>

/* ══════════════════════════════════════════════════════════════════════
 * lpkg.c — Lotus Binary Package Format (.lpkg)
 *
 * .lpkg is a tar.zst (or tar.xz fallback) archive with the layout:
 *
 *   MANIFEST          key=value metadata (name, version, deps, …)
 *   checksums.sha256  sha256 of every file under root/
 *   root/             filesystem tree installed directly into /
 *
 * Commands:
 *   lpm -Pb  <pkg…>   build package(s) then pack → .lpkg
 *   lpm -Pi  <f.lpkg> install from a local .lpkg file (skip build)
 *   lpm -Pe  <dir/>   install every *.lpkg found in a directory
 *   lpm -Pl  <f.lpkg> list contents / metadata of a .lpkg
 * ══════════════════════════════════════════════════════════════════════ */

#define LPKG_REPO_DIR "/var/cache/lpm/pkg"
#define LPKG_EXT      ".lpkg"
#define LPKG_ARCH     "x86_64"

/* ── run — thin wrapper around system(), prints cmd when verbose ─────── */
static int run(const char *cmd) {
  if (g_verbose)
    printf(C_GRAY "  $ %s\n" C_RESET, cmd);
  return system(cmd);
}

/* ── compressor — prefer zstd, fall back to xz ──────────────────────── */
static const char *compressor(void) {
  if (system("command -v zstd >/dev/null 2>&1") == 0)
    return "zstd";
  return "xz";
}

/* ── lpkg_path — build the canonical .lpkg output path ──────────────── */
static void lpkg_path(const char *name, const char *ver, const char *rel,
                      char *out, size_t sz) {
  snprintf(out, sz, "%s/%s-%s-%s-%s%s",
           LPKG_REPO_DIR, name, ver, rel, LPKG_ARCH, LPKG_EXT);
}

/* ── fmt_size — human-readable byte count (B / KiB / MiB / GiB) ─────── */
static void fmt_size(long bytes, char *out, size_t sz) {
  if      (bytes >= 1024L * 1024 * 1024)
    snprintf(out, sz, "%.2f GiB", bytes / 1024.0 / 1024.0 / 1024.0);
  else if (bytes >= 1024L * 1024)
    snprintf(out, sz, "%.2f MiB", bytes / 1024.0 / 1024.0);
  else if (bytes >= 1024)
    snprintf(out, sz, "%.1f KiB", bytes / 1024.0);
  else
    snprintf(out, sz, "%ld B", bytes);
}

/* ── dir_size — total byte usage of a directory tree via du -sb ──────── */
static long dir_size(const char *path) {
  char cmd[LPM_PATH_MAX + 64];
  snprintf(cmd, sizeof(cmd), "du -sb '%s' 2>/dev/null | cut -f1", path);
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  long sz = -1;
  fscanf(p, "%ld", &sz);
  pclose(p);
  return sz;
}

/* ══════════════════════════════════════════════════════════════════════
 * write_manifest — serialise package metadata into MANIFEST (key=value)
 *
 * Written into the staging directory before the archive is created.
 * The installed_size field lets lpm -Pi display disk usage before asking
 * the user to confirm installation.
 * ══════════════════════════════════════════════════════════════════════ */
static int write_manifest(const char *path, const Pkg *pkg,
                          long installed_bytes) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;

  fprintf(f, "name=%s\n",        pkg->pkgname);
  fprintf(f, "version=%s\n",     pkg->pkgver);
  fprintf(f, "release=%s\n",     pkg->pkgrel);
  fprintf(f, "arch=%s\n",        LPKG_ARCH);
  fprintf(f, "lpm_version=%s\n", LPM_VERSION);

  if (installed_bytes > 0)
    fprintf(f, "installed_size=%ld\n", installed_bytes);

  char ts[32];
  time_t t = time(NULL);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&t));
  fprintf(f, "packed=%s\n", ts);

  for (int i = 0; i < pkg->ndepends;    i++) fprintf(f, "depend=%s\n",     pkg->depends[i]);
  for (int i = 0; i < pkg->nmakedepends;i++) fprintf(f, "makedepend=%s\n", pkg->makedepends[i]);

  fclose(f);
  return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * LpkgMeta + parse_manifest — read MANIFEST back into a struct
 * ══════════════════════════════════════════════════════════════════════ */
typedef struct {
  char name[LPM_NAME_MAX];
  char version[LPM_VER_MAX];
  char release[16];
  char arch[32];
  char depends[LPM_MAX_DEPS][LPM_NAME_MAX];
  int  ndepends;
  long installed_size; /* 0 if not recorded in the archive */
} LpkgMeta;

static int parse_manifest(const char *path, LpkgMeta *m) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  memset(m, 0, sizeof(*m));

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = '\0';
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    char *key = line, *val = eq + 1;

    if (!strcmp(key, "name"))           strncpy(m->name,    val, LPM_NAME_MAX - 1);
    if (!strcmp(key, "version"))        strncpy(m->version, val, LPM_VER_MAX  - 1);
    if (!strcmp(key, "release"))        strncpy(m->release, val, 15);
    if (!strcmp(key, "arch"))           strncpy(m->arch,    val, 31);
    if (!strcmp(key, "installed_size")) m->installed_size = atol(val);
    if (!strcmp(key, "depend") && m->ndepends < LPM_MAX_DEPS)
      strncpy(m->depends[m->ndepends++], val, LPM_NAME_MAX - 1);
  }
  fclose(f);
  return (m->name[0] && m->version[0]) ? 0 : -1;
}

/* ── extract_source_to_ws — extract one source archive into workspace ── */
static void extract_source_to_ws(const char *fname, const char *sp,
                                  const char *ws) {
  char ex[MAX_CMD];
  if      (strstr(fname, ".tar.gz")  || strstr(fname, ".tgz"))
    snprintf(ex, sizeof(ex), "tar -xzf '%s' -C '%s' 2>/dev/null||true", sp, ws);
  else if (strstr(fname, ".tar.xz"))
    snprintf(ex, sizeof(ex), "tar -xJf '%s' -C '%s' 2>/dev/null||true", sp, ws);
  else if (strstr(fname, ".tar.bz2"))
    snprintf(ex, sizeof(ex), "tar -xjf '%s' -C '%s' 2>/dev/null||true", sp, ws);
  else if (strstr(fname, ".tar.zst"))
    snprintf(ex, sizeof(ex), "tar --zstd -xf '%s' -C '%s' 2>/dev/null||true", sp, ws);
  else if (strstr(fname, ".zip"))
    snprintf(ex, sizeof(ex), "unzip -q '%s' -d '%s' 2>/dev/null||true", sp, ws);
  else
    return; /* unknown format — skip silently */
  run(ex);
}

/* ══════════════════════════════════════════════════════════════════════
 * cmd_pack (-Pb) — build package(s) and pack into .lpkg archives
 *
 * For each package in the dep-ordered queue:
 *   1. Parse PKGBUILD (fail fast if missing)
 *   2. Skip if .lpkg already exists in LPKG_REPO_DIR
 *   3. If pkgdir is not yet built: extract sources + run build() + package()
 *   4. Measure installed size of pkgdir
 *   5. Create staging dir with MANIFEST + checksums.sha256 + root/
 *   6. Compress into .lpkg via zstd (or xz fallback)
 *
 * PERF: dep_resolve_queue_multi() used — collects all pkgs + toposort once.
 * BUG FIX: build_all=1 so installed deps (e.g. ncurses for readline)
 *          are included in the pack queue even when already on the system.
 * ══════════════════════════════════════════════════════════════════════ */
void cmd_pack(int argc, char **argv) {
  check_root();
  init_dirs();

  LpmConfig cfg;
  lpm_config_load(LPM_CONF_FILE, &cfg);

  if (argc == 0)
    die("No package specified.\nUsage: lpm -Pb <package...>");

  util_mkdirp(LPKG_REPO_DIR, 0755);
  const char *comp = compressor();

  LpmFlags flags;
  char *pkgnames[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgnames, 256);
  if (npkgs == 0)
    die("No package specified.\nUsage: lpm -Pb <package...>");

  /* PERF: resolve all deps in one pass, toposort once.
   * build_all=1: include deps already installed on the system — needed
   * because -Pb is creating self-contained .lpkg archives, not installing. */
  char queue[256][MAX_STR];
  int nqueue = dep_resolve_queue_multi(pkgnames, npkgs, queue, 256, 1);

  printf(C_CYAN "::" C_RESET " Packages to build ("
         C_BOLD "%d" C_RESET "):\n", nqueue);
  for (int i = 0; i < nqueue; i++)
    printf("    " C_CYAN "%d." C_RESET " %s\n", i + 1, queue[i]);
  printf("\n");

  if (!flags.no_confirm)
    if (!confirm("Build and pack these packages? [" C_GREEN "Yes" C_RESET
                 "/" C_RED "No" C_RESET "] ")) {
      printf("Aborted.\n");
      return;
    }

  for (int a = 0; a < nqueue; a++) {
    const char *pkgname = queue[a];

    char pbfile[LPM_PATH_MAX];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, pkgname);
    struct stat st;
    if (stat(pbfile, &st) != 0)
      die("No PKGBUILD for '%s'. Run 'lpm -Sy %s' first.", pkgname, pkgname);

    Pkg pkg;
    if (pkgbuild_parse(pbfile, &pkg) != 0)
      die("Failed to parse pkgbuild_%s", pkgname);

    char outpath[LPM_PATH_MAX];
    lpkg_path(pkg.pkgname, pkg.pkgver, pkg.pkgrel, outpath, sizeof(outpath));

    /* skip if archive already exists */
    if (stat(outpath, &st) == 0) {
      printf(C_CYAN "  ->" C_RESET " %s already packed, skipping\n", outpath);
      continue;
    }

    printf(C_BOLD "\n==> Packing %s %s-%s" C_RESET "\n",
           pkg.pkgname, pkg.pkgver, pkg.pkgrel);

    char ws[LPM_PATH_MAX];
    snprintf(ws, sizeof(ws), "%s/%s", LPM_BUILD_DIR, pkg.pkgname);
    util_mkdirp(ws, 0755);

    char pkgdir[LPM_PATH_MAX];
    snprintf(pkgdir, sizeof(pkgdir), "%s/pkg", ws);

    /* check if package() was already run (marker file or non-empty pkgdir) */
    char built_marker[LPM_PATH_MAX];
    snprintf(built_marker, sizeof(built_marker), "%s/.lpm_built", pkgdir);

    int pkgdir_ready = (stat(built_marker, &st) == 0);
    if (!pkgdir_ready) {
      DIR *pd = opendir(pkgdir);
      if (pd) {
        struct dirent *pe;
        while ((pe = readdir(pd)))
          if (strcmp(pe->d_name, ".") && strcmp(pe->d_name, ".."))
            { pkgdir_ready = 1; break; }
        closedir(pd);
      }
    }

    if (!pkgdir_ready) {
      printf(C_CYAN "::" C_RESET " Building %s first...\n", pkg.pkgname);

      char pkg_log[LPM_PATH_MAX];
      pkg_log_path(pkg.pkgname, pkg_log, sizeof(pkg_log));

      /* extract sources (copy from /sources/ mirror if available) */
      for (int si = 0; si < pkg.nsources; si++) {
        if (!pkg.source[si][0]) continue;
        char *fn = strrchr(pkg.source[si], '/');
        if (!fn) continue;
        fn++;

        char sp[LPM_PATH_MAX];
        snprintf(sp, sizeof(sp), "%s/%s", ws, fn);

        struct stat ss;
        if (stat(sp, &ss) != 0) {
          /* not in workspace — try offline mirror */
          char ls[LPM_PATH_MAX];
          snprintf(ls, sizeof(ls), "/sources/%s", fn);
          if (stat(ls, &ss) == 0) {
            char cp[LPM_PATH_MAX * 2 + 16];
            snprintf(cp, sizeof(cp), "cp '%s' '%s'", ls, sp);
            run(cp);
          } else
            continue;
        }
        extract_source_to_ws(fn, sp, ws);
      }

      /* run build() */
      char build_cmd[MAX_CMD];
      snprintf(build_cmd, sizeof(build_cmd),
               "bash -c 'source \"%s\" && cd \"%s\""
               " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
               " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
               " && build' > \"%s\" 2>&1",
               pbfile, ws,
               cfg.cflags, cfg.cxxflags, cfg.ldflags,
               cfg.makeflags, cfg.cc, cfg.cxx, pkg_log);
      if (run(build_cmd) != 0)
        die("Build failed for %s — see %s", pkg.pkgname, pkg_log);

      /* clean and recreate pkgdir, then run package() */
      char mk[LPM_PATH_MAX * 2 + 32];
      snprintf(mk, sizeof(mk), "rm -rf '%s' && mkdir -p '%s'", pkgdir, pkgdir);
      run(mk);

      char inst_cmd[MAX_CMD];
      snprintf(inst_cmd, sizeof(inst_cmd),
               "bash -c 'source \"%s\" && cd \"%s\""
               " && export CFLAGS=\"%s\" CXXFLAGS=\"%s\" LDFLAGS=\"%s\""
               " MAKEFLAGS=\"%s\" CC=\"%s\" CXX=\"%s\""
               " pkgdir=\"%s\" && package' >> \"%s\" 2>&1",
               pbfile, ws,
               cfg.cflags, cfg.cxxflags, cfg.ldflags,
               cfg.makeflags, cfg.cc, cfg.cxx, pkgdir, pkg_log);
      if (run(inst_cmd) != 0)
        die("package() failed for %s — see %s", pkg.pkgname, pkg_log);

      /* leave a marker so re-runs can skip the build phase */
      FILE *mf = fopen(built_marker, "w");
      if (mf) fclose(mf);
    } else {
      printf(C_CYAN "  ->" C_RESET " Already built, reusing pkgdir\n");
    }

    /* measure installed size before compressing */
    long installed_bytes = dir_size(pkgdir);

    /* create staging directory: MANIFEST + checksums + root/ */
    char stage[LPM_PATH_MAX];
    snprintf(stage, sizeof(stage), "%s/.lpkg_stage", ws);
    char rm_stage[LPM_PATH_MAX + 16];
    snprintf(rm_stage, sizeof(rm_stage), "rm -rf '%s'", stage);
    run(rm_stage);
    util_mkdirp(stage, 0755);

    char manifest[LPM_PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/MANIFEST", stage);
    if (write_manifest(manifest, &pkg, installed_bytes) != 0)
      die("Failed to write MANIFEST");

    /* copy pkgdir → stage/root/ */
    char stage_root[LPM_PATH_MAX];
    snprintf(stage_root, sizeof(stage_root), "%s/root", stage);
    char cp_cmd[LPM_PATH_MAX * 2 + 32];
    snprintf(cp_cmd, sizeof(cp_cmd), "cp -a '%s' '%s'", pkgdir, stage_root);
    run(cp_cmd);

    /* remove the build marker from the archive root */
    char rm_marker[LPM_PATH_MAX];
    snprintf(rm_marker, sizeof(rm_marker),
             "find '%s/root' -name '.lpm_built' -delete", stage);
    run(rm_marker);

    /* generate checksums.sha256 for every file in root/ */
    char cksum_path[LPM_PATH_MAX];
    snprintf(cksum_path, sizeof(cksum_path), "%s/checksums.sha256", stage);
    char cksum_cmd[LPM_PATH_MAX * 2 + 128];
    snprintf(cksum_cmd, sizeof(cksum_cmd),
             "find '%s/root' -type f ! -name '.lpm_built' | sort"
             " | xargs sha256sum 2>/dev/null"
             " | sed \"s|%s/root/||g\" > '%s'",
             stage, stage, cksum_path);
    run(cksum_cmd);

    /* compress the staging dir into the final .lpkg */
    char tar_cmd[LPM_PATH_MAX * 3 + 128];
    if (!strcmp(comp, "zstd"))
      snprintf(tar_cmd, sizeof(tar_cmd),
               "tar -C '%s' -c . | zstd -T0 -19 -o '%s'", stage, outpath);
    else
      snprintf(tar_cmd, sizeof(tar_cmd),
               "tar -C '%s' -cJf '%s' .", stage, outpath);

    printf(C_CYAN "::" C_RESET " Compressing → %s\n", outpath);
    if (run(tar_cmd) != 0)
      die("Failed to create %s", outpath);

    run(rm_stage);

    /* report both compressed (download) and uncompressed (installed) sizes */
    struct stat ps;
    char dl_str[32] = "?", inst_str[32] = "?";
    if (stat(outpath, &ps) == 0) fmt_size(ps.st_size,      dl_str,   sizeof(dl_str));
    if (installed_bytes > 0)     fmt_size(installed_bytes, inst_str, sizeof(inst_str));

    printf(C_GREEN "==> Packed:" C_RESET " %s\n"
           "     Download size:  %s\n"
           "     Installed size: %s\n",
           outpath, dl_str, inst_str);
    lpm_log("Packed %s %s-%s → %s (dl=%s inst=%s)",
            pkg.pkgname, pkg.pkgver, pkg.pkgrel, outpath, dl_str, inst_str);
  }
}

/* ══════════════════════════════════════════════════════════════════════
 * lpkg_install_one — install a single .lpkg archive into the system
 *
 * Steps:
 *   1. Extract archive into a temp directory
 *   2. Parse and validate MANIFEST (arch check, version compare)
 *   3. Display package info (name, version, sizes, deps)
 *   4. Prompt for confirmation (unless no_confirm)
 *   5. Verify checksums.sha256
 *   6. Run safety checks (toolchain + file conflicts)
 *   7. Check available disk space
 *   8. Merge root/ into / via cp -a
 *   9. Register in the lpm database
 * ══════════════════════════════════════════════════════════════════════ */
static int lpkg_install_one(const char *lpkg_path_arg, int no_confirm) {
  (void)no_confirm;  /* batch-confirmed by caller; kept for API compat */
  struct stat st;
  if (stat(lpkg_path_arg, &st) != 0) {
    fprintf(stderr, C_RED "error:" C_RESET " file not found: %s\n", lpkg_path_arg);
    return -1;
  }

  printf(C_BOLD "\n==> Installing %s" C_RESET "\n",
         strrchr(lpkg_path_arg, '/') ? strrchr(lpkg_path_arg, '/') + 1
                                     : lpkg_path_arg);

  /* extract into a per-pid temp directory */
  char tmpdir[LPM_PATH_MAX];
  snprintf(tmpdir, sizeof(tmpdir), "/tmp/lpm_lpkg_%d", (int)getpid());
  util_mkdirp(tmpdir, 0755);

  char ext_cmd[LPM_PATH_MAX * 2 + 64];
  snprintf(ext_cmd, sizeof(ext_cmd),
           "tar -C '%s' --zstd -xf '%s' 2>/dev/null"
           " || tar -C '%s' -xf '%s' 2>/dev/null",
           tmpdir, lpkg_path_arg, tmpdir, lpkg_path_arg);
  if (run(ext_cmd) != 0) {
    fprintf(stderr, C_RED "error:" C_RESET " failed to extract %s\n", lpkg_path_arg);
    util_rmrf(tmpdir);
    return -1;
  }

  /* parse and validate MANIFEST */
  char manifest[LPM_PATH_MAX];
  snprintf(manifest, sizeof(manifest), "%s/MANIFEST", tmpdir);
  LpkgMeta meta;
  if (parse_manifest(manifest, &meta) != 0) {
    fprintf(stderr, C_RED "error:" C_RESET " invalid MANIFEST in %s\n", lpkg_path_arg);
    util_rmrf(tmpdir);
    return -1;
  }

  /* per-package disk check before merge */
  {
    long free_bytes = util_disk_free("/");
    long need = meta.installed_size > 0
                ? (long)(meta.installed_size * 1.10)
                : 0;
    if (free_bytes > 0 && need > 0 && free_bytes < need) {
      char free_str[32], need_str[32];
      fmt_size(free_bytes, free_str, sizeof(free_str));
      fmt_size(need,       need_str, sizeof(need_str));
      fprintf(stderr, C_RED "error:" C_RESET
              " No space left installing %s"
              " — need %s, have %s\n",
              meta.name, need_str, free_str);
      util_rmrf(tmpdir);
      return -1;
    }
  }

  /* architecture check */
  if (strcmp(meta.arch, LPKG_ARCH) != 0) {
    fprintf(stderr, C_RED "error:" C_RESET
            " arch mismatch: package is %s, system is %s\n",
            meta.arch, LPKG_ARCH);
    util_rmrf(tmpdir);
    return -1;
  }

  /* skip or show upgrade notice if already installed */
  if (db_is_installed(meta.name)) {
    char *inst_ver = db_get_version(meta.name);
    if (inst_ver) {
      char full_new[LPM_VER_MAX + 16];
      snprintf(full_new, sizeof(full_new), "%s-%s", meta.version, meta.release);
      int cmp = version_compare(inst_ver, full_new);
      if (cmp >= 0) {
        printf(C_YELLOW "  ->" C_RESET
               " %s %s already installed (have %s), skipping\n",
               meta.name, full_new, inst_ver);
        free(inst_ver);
        util_rmrf(tmpdir);
        return 0;
      }
      printf(C_CYAN "  ->" C_RESET " Upgrading %s: %s → %s\n",
             meta.name, inst_ver, full_new);
      free(inst_ver);
    }
  }

  /* confirmation already handled by caller (batch confirm)
   * no_confirm=1 is always passed after the table prompt           */

  /* checksum verification */
  char cksum_file[LPM_PATH_MAX];
  snprintf(cksum_file, sizeof(cksum_file), "%s/checksums.sha256", tmpdir);
  if (stat(cksum_file, &st) == 0) {
    printf(C_CYAN "::" C_RESET " Verifying checksums...\n");
    char verify_cmd[LPM_PATH_MAX * 2 + 64];
    snprintf(verify_cmd, sizeof(verify_cmd),
             "cd '%s/root' && sha256sum -c '%s' --quiet 2>&1",
             tmpdir, cksum_file);
    if (run(verify_cmd) != 0) {
      fprintf(stderr, C_RED "error:" C_RESET
              " checksum verification failed for %s\n", meta.name);
      util_rmrf(tmpdir);
      return -1;
    }
    printf(C_GREEN "  ok" C_RESET " checksums passed\n");
  }

  char root_dir[LPM_PATH_MAX];
  snprintf(root_dir, sizeof(root_dir), "%s/root", tmpdir);

  /* safety checks: toolchain protection + file conflicts */
  if (safety_check_toolchain(root_dir, meta.name) != 0)
    { util_rmrf(tmpdir); return -1; }
  if (safety_check_file_conflicts(root_dir, meta.name, 0) != 0)
    { util_rmrf(tmpdir); return -1; }

  /* disk space check */
  long needed_kb = (meta.installed_size > 0)
                   ? meta.installed_size / 1024
                   : st.st_size / 512;
  long free_root = util_disk_free("/");
  if (free_root >= 0 && free_root < needed_kb) {
    fprintf(stderr, C_RED "error:" C_RESET
            " not enough disk space (need ~%ld KiB, have %ld KiB)\n",
            needed_kb, free_root);
    util_rmrf(tmpdir);
    return -1;
  }

  /* merge filesystem tree into / */
  printf(C_CYAN "  ->" C_RESET " Merging into /...\n");
  char merge_cmd[LPM_PATH_MAX * 2 + 32];
  snprintf(merge_cmd, sizeof(merge_cmd),
           "cp -a --remove-destination '%s/root'/. /", tmpdir);
  if (run(merge_cmd) != 0) {
    fprintf(stderr, C_RED "error:" C_RESET " merge failed for %s\n", meta.name);
    util_rmrf(tmpdir);
    return -1;
  }

  /* register in database */
  db_files_save(meta.name, root_dir);
  db_add(meta.name, meta.version, meta.release);
  lpm_audit("lpkg-install: %s %s-%s from %s",
            meta.name, meta.version, meta.release, lpkg_path_arg);

  util_rmrf(tmpdir);

  printf(C_GREEN "==> Installed" C_RESET " %s %s-%s (from binary)\n",
         meta.name, meta.version, meta.release);
  lpm_log("lpkg install: %s %s-%s", meta.name, meta.version, meta.release);
  return 0;
}

/* ── resolve_lpkg_path — resolve package name or path to a .lpkg file ── *
 * If lpkg_arg is an existing file path, returns it unchanged.           *
 * Otherwise searches LPKG_REPO_DIR for a filename starting with         *
 * "<lpkg_arg>-" and ending in LPKG_EXT.                                 *
 * Returns 1 on success (out filled), 0 if not found.                   */
static int resolve_lpkg_path(const char *lpkg_arg, char *out, size_t outsz) {
  struct stat rst;
  if (stat(lpkg_arg, &rst) == 0) {
    strncpy(out, lpkg_arg, outsz - 1);
    return 1;
  }

  DIR *d = opendir(LPKG_REPO_DIR);
  if (!d) return 0;

  size_t prefix_len = strlen(lpkg_arg);
  struct dirent *ent;
  while ((ent = readdir(d))) {
    size_t nl = strlen(ent->d_name);
    if (nl < 5 || strcmp(ent->d_name + nl - 5, LPKG_EXT)) continue;
    if (strncmp(ent->d_name, lpkg_arg, prefix_len) == 0 &&
        ent->d_name[prefix_len] == '-') {
      snprintf(out, outsz, "%s/%s", LPKG_REPO_DIR, ent->d_name);
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}

/* ── cmd_pkginstall (-Pi) ────────────────────────────────────────────── *
 * Install one or more .lpkg files given as filenames or package names.  *
 * Automatically resolves and installs missing deps first.               */
void cmd_pkginstall(int argc, char **argv) {
  check_root();
  init_dirs();

  if (argc == 0)
    die("No .lpkg specified.\nUsage: lpm -Pi <file.lpkg> [...]");

  LpmFlags flags;
  char *pkgs[256];
  int npkgs = lpm_parse_flags(argc, argv, &flags, pkgs, 256);
  if (npkgs == 0)
    die("No .lpkg file specified.");

  /* ── build install queue with dep resolution ── */
  /* collect all package names (strip path + ext if full path given) */
  char names[256][LPM_NAME_MAX];
  int  nnames = 0;
  char paths[256][LPM_PATH_MAX];  /* original resolved paths */

  for (int i = 0; i < npkgs && nnames < 256; i++) {
    char resolved[LPM_PATH_MAX];
    if (!resolve_lpkg_path(pkgs[i], resolved, sizeof(resolved))) {
      fprintf(stderr, C_RED "error:" C_RESET
              " no .lpkg found for '%s'\n", pkgs[i]);
      continue;
    }
    strncpy(paths[nnames], resolved, LPM_PATH_MAX - 1);

    /* derive package name from filename: <name>-<ver>-<rel>-<arch>.lpkg */
    const char *base = strrchr(resolved, '/');
    base = base ? base + 1 : resolved;
    char tmp[LPM_NAME_MAX];
    strncpy(tmp, base, LPM_NAME_MAX - 1);
    /* strip trailing -<ver>-<rel>-<arch>.lpkg — find first dash */
    char *dash = strchr(tmp, '-');
    if (dash) *dash = '\0';
    strncpy(names[nnames], tmp, LPM_NAME_MAX - 1);
    nnames++;
  }

  if (nnames == 0) return;

  /* resolve dep queue — only packages NOT yet installed */
  char queue[256][MAX_STR];
  int  nqueue = 0;

  for (int i = 0; i < nnames; i++) {
    char subq[256][MAX_STR];
    int nsub = dep_resolve_queue(names[i], subq, 256, 0);
    for (int j = 0; j < nsub; j++) {
      if (db_is_installed(subq[j])) continue;  /* already on system */
      int dup = 0;
      for (int k = 0; k < nqueue; k++)
        if (!strcmp(queue[k], subq[j])) { dup = 1; break; }
      if (!dup) strncpy(queue[nqueue++], subq[j], MAX_STR - 1);
    }
    if (!db_is_installed(names[i])) {
      int dup = 0;
      for (int k = 0; k < nqueue; k++)
        if (!strcmp(queue[k], names[i])) { dup = 1; break; }
      if (!dup) strncpy(queue[nqueue++], names[i], MAX_STR - 1);
    }
  }

  if (nqueue == 0) {
    printf(C_GREEN "::" C_RESET " All packages already installed.\n");
    return;
  }

  /* ── gather metadata for table display ─────────────────────────── */
  typedef struct {
    char  name[LPM_NAME_MAX];
    char  version[LPM_VER_MAX + 16];
    char  lpath[LPM_PATH_MAX];
    long  dl_size;
    long  inst_size;
    int   missing;
  } InstEntry;

  InstEntry *entries = calloc(nqueue, sizeof(InstEntry));
  if (!entries) die("out of memory");

  int missing = 0;
  long total_dl = 0, total_inst = 0;

  for (int i = 0; i < nqueue; i++) {
    InstEntry *e = &entries[i];
    strncpy(e->name, queue[i], LPM_NAME_MAX - 1);

    if (!resolve_lpkg_path(queue[i], e->lpath, sizeof(e->lpath))) {
      e->missing = 1; missing++;
      continue;
    }

    /* get .lpkg size from stat */
    struct stat lst;
    if (stat(e->lpath, &lst) == 0) e->dl_size = lst.st_size;

    /* peek into MANIFEST for installed_size + version */
    char tmpd[LPM_PATH_MAX];
    snprintf(tmpd, sizeof(tmpd), "/tmp/lpm_peek_%d_%d", (int)getpid(), i);
    util_mkdirp(tmpd, 0755);
    char peek[LPM_PATH_MAX * 2 + 64];
    snprintf(peek, sizeof(peek),
             "tar -C '%s' --zstd -xf '%s' MANIFEST 2>/dev/null"
             " || tar -C '%s' -xf '%s' MANIFEST 2>/dev/null",
             tmpd, e->lpath, tmpd, e->lpath);
    if (run(peek) == 0) {
      char mpath[LPM_PATH_MAX];
      snprintf(mpath, sizeof(mpath), "%s/MANIFEST", tmpd);
      LpkgMeta meta; memset(&meta, 0, sizeof(meta));
      if (parse_manifest(mpath, &meta) == 0) {
        snprintf(e->version, sizeof(e->version),
                 "%s-%s", meta.version, meta.release);
        e->inst_size = meta.installed_size;
      }
    }
    util_rmrf(tmpd);

    total_dl   += e->dl_size;
    total_inst += e->inst_size;
  }

  /* ── print table ─────────────────────────────────────────────────── */
  printf(C_CYAN "::" C_RESET C_BOLD
         " Packages to install (%d):\n\n" C_RESET, nqueue);

  printf("  %-8s  %-24s  %-16s  %-12s  %s\n",
         "Type", "Package", "Version", "Download", "Install size");
  printf("  %-8s  %-24s  %-16s  %-12s  %s\n",
         "------", "-------", "-------", "--------", "------------");

  for (int i = 0; i < nqueue; i++) {
    InstEntry *e = &entries[i];
    char dl_str[32], inst_str[32];
    fmt_size(e->dl_size,   dl_str,   sizeof(dl_str));
    if (e->inst_size > 0)
      fmt_size(e->inst_size, inst_str, sizeof(inst_str));
    else
      snprintf(inst_str, sizeof(inst_str), C_GRAY "—" C_RESET);

    if (e->missing) {
      printf("  %-8s  %-24s  %-16s  %-12s  %s\n",
             "[" C_RED "???" C_RESET "]",
             e->name, "—", "—",
             C_RED "no .lpkg!" C_RESET);
    } else {
      printf("  [" C_BLUE "bin" C_RESET "]   %-24s  %-16s  %-12s  %s\n",
             e->name,
             e->version[0] ? e->version : "—",
             dl_str, inst_str);
    }
  }

  /* summary */
  char tdl[32], tinst[32];
  fmt_size(total_dl,   tdl,   sizeof(tdl));
  fmt_size(total_inst, tinst, sizeof(tinst));
  printf("\n");
  printf("  Total download:    %s\n", tdl);
  printf("  Total install:     %s\n", tinst);

  /* disk space check */
  long free_bytes = util_disk_free("/");
  if (free_bytes > 0) {
    char free_str[32];
    fmt_size(free_bytes, free_str, sizeof(free_str));
    long needed = (long)(total_inst * 1.10);
    if (free_bytes < needed) {
      char need_str[32];
      fmt_size(needed, need_str, sizeof(need_str));
      printf("  Free space:        " C_RED "%s  ✗ Not enough!" C_RESET "\n\n",
             free_str);
      fprintf(stderr, C_RED "error:" C_RESET
              " No space left — need %s, have %s\n", need_str, free_str);
      free(entries);
      return;
    }
    printf("  Free space:        " C_GREEN "%s  ✓ OK" C_RESET "\n", free_str);
  }
  printf("\n");

  if (missing > 0) {
    fprintf(stderr, C_RED "error:" C_RESET
            " %d dep(s) missing .lpkg — run 'lpm -Pb' first\n", missing);
    free(entries);
    return;
  }

  /* ── single confirm ──────────────────────────────────────────────── */
  if (!flags.no_confirm)
    if (!confirm("Proceed with installation? [" C_GREEN "Y" C_RESET
                 "/" C_RED "n" C_RESET "] ")) {
      printf("Aborted.\n");
      free(entries);
      return;
    }

  free(entries);

  int failed = 0;
  for (int i = 0; i < nqueue; i++) {
    char lpath[LPM_PATH_MAX];
    resolve_lpkg_path(queue[i], lpath, sizeof(lpath));
    printf(C_CYAN "(%d/%d)" C_RESET C_BOLD " Installing %s" C_RESET "\n",
           i + 1, nqueue, queue[i]);
    if (lpkg_install_one(lpath, 1) != 0)  /* confirmed above */
      failed++;
  }
  printf("\n");
  if (!failed)
    printf(C_GREEN "::" C_RESET " Installation complete.\n");

  if (failed > 0)
    fprintf(stderr, C_RED "error:" C_RESET
            " %d package(s) failed to install\n", failed);
}

/* ── cmd_pkginstall_dir (-Pe) ────────────────────────────────────────── *
 * Install every *.lpkg found in the given directory.                   *
 * Asks a single batch confirmation, then installs each file silently.  */
void cmd_pkginstall_dir(int argc, char **argv) {
  check_root();
  init_dirs();

  if (argc == 0)
    die("No directory specified.\nUsage: lpm -Pe <dir/>");

  LpmFlags flags;
  char *args[256];
  int nargs = lpm_parse_flags(argc, argv, &flags, args, 256);
  const char *dir = (nargs > 0) ? args[0] : argv[0];

  DIR *d = opendir(dir);
  if (!d)
    die("Cannot open directory: %s", dir);

  char lpkgs[256][LPM_PATH_MAX];
  int n = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) && n < 256) {
    size_t len = strlen(ent->d_name);
    if (len < 5 || strcmp(ent->d_name + len - 5, LPKG_EXT) != 0) continue;
    snprintf(lpkgs[n++], LPM_PATH_MAX, "%s/%s", dir, ent->d_name);
  }
  closedir(d);

  if (n == 0) {
    printf(C_YELLOW "warning:" C_RESET " no .lpkg files in %s\n", dir);
    return;
  }

  /* sort alphabetically so deps install before dependents when named correctly */
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (strcmp(lpkgs[i], lpkgs[j]) > 0) {
        char tmp[LPM_PATH_MAX];
        memcpy(tmp,     lpkgs[i], LPM_PATH_MAX);
        memcpy(lpkgs[i], lpkgs[j], LPM_PATH_MAX);
        memcpy(lpkgs[j], tmp,      LPM_PATH_MAX);
      }

  printf(C_CYAN "::" C_RESET " Found " C_BOLD "%d" C_RESET
         " package(s) in %s\n\n", n, dir);

  if (!flags.no_confirm)
    if (!confirm("Install all? [" C_GREEN "Yes" C_RESET
                 "/" C_RED "No" C_RESET "] ")) {
      printf("Aborted.\n");
      return;
    }

  int failed = 0;
  for (int i = 0; i < n; i++) {
    /* batch already confirmed above — pass no_confirm=1 for each file */
    if (lpkg_install_one(lpkgs[i], 1) != 0)
      failed++;
  }

  printf("\n" C_CYAN "::" C_RESET " Done: %d installed", n - failed);
  if (failed > 0) printf(", " C_RED "%d failed" C_RESET, failed);
  printf("\n");
}

/* ── cmd_pkglist (-Pl) ───────────────────────────────────────────────── *
 * Print metadata and file list for one or more .lpkg archives.         *
 * Accepts both full file paths and bare package names.                 */
void cmd_pkglist(int argc, char **argv) {
  if (argc == 0)
    die("No .lpkg specified.\nUsage: lpm -Pl <file.lpkg|pkgname>");

  for (int a = 0; a < argc; a++) {
    char resolved[LPM_PATH_MAX];
    if (!resolve_lpkg_path(argv[a], resolved, sizeof(resolved))) {
      fprintf(stderr, C_RED "error:" C_RESET
              " no .lpkg found for '%s' in %s\n", argv[a], LPKG_REPO_DIR);
      continue;
    }

    const char *lpkg_file = resolved;

    /* extract into a unique temp dir */
    char tmpdir[LPM_PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/lpm_lpkglist_%d_%d",
             (int)getpid(), a);

    char rm_cmd[LPM_PATH_MAX + 16];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
    system(rm_cmd);

    if (util_mkdirp(tmpdir, 0755) != 0) {
      fprintf(stderr, C_RED "error:" C_RESET
              " cannot create tmpdir %s\n", tmpdir);
      continue;
    }

    char ext_cmd[LPM_PATH_MAX * 2 + 64];
    snprintf(ext_cmd, sizeof(ext_cmd),
             "tar -C '%s' --zstd -xf '%s' 2>/dev/null"
             " || tar -C '%s' -xf '%s' 2>/dev/null",
             tmpdir, lpkg_file, tmpdir, lpkg_file);
    system(ext_cmd);

    char manifest_path[LPM_PATH_MAX];
    snprintf(manifest_path, sizeof(manifest_path), "%s/MANIFEST", tmpdir);
    struct stat mst;
    if (stat(manifest_path, &mst) != 0) {
      fprintf(stderr, C_RED "error:" C_RESET
              " cannot read MANIFEST from %s\n", lpkg_file);
      system(rm_cmd);
      continue;
    }

    LpkgMeta meta;
    if (parse_manifest(manifest_path, &meta) != 0) {
      fprintf(stderr, C_RED "error:" C_RESET
              " invalid MANIFEST in %s\n", lpkg_file);
      util_rmrf(tmpdir);
      continue;
    }

    /* print metadata header */
    printf(C_BOLD "── %s ─────────────────────\n" C_RESET,
           strrchr(lpkg_file, '/') ? strrchr(lpkg_file, '/') + 1 : lpkg_file);
    printf("  name:      %s\n",      meta.name);
    printf("  version:   %s-%s\n",   meta.version, meta.release);
    printf("  arch:      %s\n",      meta.arch);

    struct stat ps;
    char dl_str[32] = "?", inst_str[32] = "?";
    if (stat(lpkg_file, &ps) == 0)
      fmt_size(ps.st_size, dl_str, sizeof(dl_str));
    printf("  download:  %s\n", dl_str);

    if (meta.installed_size > 0) {
      fmt_size(meta.installed_size, inst_str, sizeof(inst_str));
      printf("  installed: %s\n", inst_str);
    }

    if (meta.ndepends > 0) {
      printf("  depends:");
      for (int i = 0; i < meta.ndepends; i++) printf(" %s", meta.depends[i]);
      printf("\n");
    }

    /* print file list from checksums.sha256 */
    char cksum[LPM_PATH_MAX];
    snprintf(cksum, sizeof(cksum), "%s/checksums.sha256", tmpdir);
    FILE *f = fopen(cksum, "r");
    if (f) {
      printf("  files:\n");
      char line[LPM_PATH_MAX + 70];
      int nf = 0;
      while (fgets(line, sizeof(line), f)) {
        char *path = strchr(line, ' ');
        if (!path) continue;
        while (*path == ' ') path++;
        if (path[0] == '.') path++;
        path[strcspn(path, "\n")] = '\0';
        printf("    %s\n", path);
        nf++;
      }
      fclose(f);
      printf("  total: %d file(s)\n", nf);
    }
    printf("\n");

    util_rmrf(tmpdir);
  }
}
