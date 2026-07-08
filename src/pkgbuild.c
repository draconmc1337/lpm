#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* ── helpers ─────────────────────────────────────────────────────────────── */

/* run bash snippet, collect stdout lines into out[], return count.
 * out is a flat pointer into a 2D array whose row width is rowsz — the
 * caller passes e.g. (char *)pkg->depends, LPM_NAME_MAX or
 * (char *)pkg->source, LPM_PATH_MAX. Reading through a fixed-size local
 * line buffer (independent of rowsz) before copying into the row means
 * this is safe for both directions: it can't overflow a narrow row
 * (previously always read up to MAX_STR/4096 bytes regardless of the
 * actual row width — harmless for source[][4096], but genuinely unsafe
 * for e.g. depends[][128]), and it can't silently truncate a wide one. */
static int bash_array(const char *pbfile, const char *varname,
                      char *out, size_t rowsz, int maxn) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "bash -c 'source \"%s\" 2>/dev/null; "
           "for _x in \"${%s[@]}\"; do printf \"%%s\\n\" \"$_x\"; done'",
           pbfile, varname);
  FILE *p = popen(cmd, "r");
  if (!p)
    return 0;
  char line[LPM_PATH_MAX];
  int n = 0;
  while (n < maxn && fgets(line, sizeof(line), p)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0]) {
      char *dst = out + (size_t)n * rowsz;
      strncpy(dst, line, rowsz - 1);
      dst[rowsz - 1] = '\0';
      n++;
    }
  }
  pclose(p);
  return n;
}

static void bash_scalar(const char *pbfile, const char *varname, char *out,
                        size_t outsz) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "bash -c 'source \"%s\" 2>/dev/null; printf \"%%s\" \"${%s}\"'",
           pbfile, varname);
  FILE *p = popen(cmd, "r");
  out[0] = '\0';
  if (!p)
    return;
  (void)fgets(out, (int)outsz, p);
  out[strcspn(out, "\n")] = '\0';
  pclose(p);
}

static int bash_func_exists(const char *pbfile, const char *fname) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "bash -c 'source \"%s\" 2>/dev/null; declare -f %s &>/dev/null'",
           pbfile, fname);
  return (system(cmd) == 0);
}

/* ── pkgbuild_parse ──────────────────────────────────────────────────────── */
int pkgbuild_parse(const char *pbfile, Pkg *pkg) {
  struct stat st;
  if (stat(pbfile, &st) != 0)
    return -1;

  memset(pkg, 0, sizeof(*pkg));
  strncpy(pkg->pbfile, pbfile, MAX_STR - 1);

  bash_scalar(pbfile, "pkgname", pkg->pkgname, MAX_STR);
  bash_scalar(pbfile, "pkgver", pkg->pkgver, MAX_STR);
  bash_scalar(pbfile, "pkgrel", pkg->pkgrel, MAX_STR);

  pkg->ndepends = bash_array(pbfile, "depends",
                             (char *)pkg->depends, LPM_NAME_MAX, MAX_DEPS);
  pkg->nrecommends = bash_array(pbfile, "recommends",
                                (char *)pkg->recommends, LPM_NAME_MAX, MAX_DEPS);
  pkg->nmakedepends = bash_array(pbfile, "makedepends",
                                 (char *)pkg->makedepends, LPM_NAME_MAX, MAX_DEPS);
  pkg->nreplaces = bash_array(pbfile, "replaces",
                              (char *)pkg->replaces, LPM_NAME_MAX, MAX_DEPS);
  pkg->nconflicts = bash_array(pbfile, "conflicts",
                               (char *)pkg->conflicts, LPM_NAME_MAX, MAX_DEPS);
  pkg->nbackup = bash_array(pbfile, "backup",
                            (char *)pkg->backup, LPM_PATH_MAX, LPM_MAX_BACKUP);

  /* sources=(...) — array only. The old scalar source/source2/source3
   * convention no longer exists in the Lotus PKGBUILD spec. */
  pkg->nsources = bash_array(pbfile, "sources",
                             (char *)pkg->source, LPM_PATH_MAX, MAX_SRCS);

  /* checksums=(...) — one "algo:hex" (or "SKIP") entry per source[],
   * same index. The old per-algorithm sha256sums/sha512sums/md5sums
   * scalars no longer exist in the Lotus PKGBUILD spec. */
  pkg->nchecksums = bash_array(pbfile, "checksums",
                               (char *)pkg->checksums, 200, MAX_SRCS);

  pkg->has_check = bash_func_exists(pbfile, "check");
  pkg->has_uninstall = bash_func_exists(pbfile, "uninstall");

  return 0;
}

/* ── dep_satisfied ───────────────────────────────────────────────────────── */
int dep_satisfied(const char *spec) {
  char pkgname[MAX_STR];
  char op[4] = "";
  char ver_need[MAX_STR] = "";

  /* parse "name>=ver" / "name<=ver" / "name=ver" / "name" */
  const char *p = spec;
  int ni = 0;
  while (*p && *p != '>' && *p != '<' && *p != '=')
    pkgname[ni++] = *p++;
  pkgname[ni] = '\0';

  if (*p) {
    int oi = 0;
    while (*p == '>' || *p == '<' || *p == '=')
      op[oi++] = *p++;
    op[oi] = '\0';
    strncpy(ver_need, p, MAX_STR - 1);
  }

  if (!db_is_installed(pkgname))
    return 0;
  if (op[0] == '\0')
    return 1; /* no version constraint */

  char *installed = db_get_version(pkgname);
  if (!installed)
    return 1; /* old DB format, assume ok */

  /* installed is "ver-rel", strip "-rel" */
  char have[MAX_STR];
  strncpy(have, installed, MAX_STR - 1);
  free(installed);
  char *dash = strrchr(have, '-');
  if (dash)
    *dash = '\0';

  int ok = 0;
  if (strcmp(op, ">=") == 0)
    ok = version_gte(have, ver_need);
  else if (strcmp(op, "<=") == 0)
    ok = version_gte(ver_need, have);
  else if (strcmp(op, "=") == 0)
    ok = (strcmp(have, ver_need) == 0);

  return ok;
}

/* ── reverse_deps ────────────────────────────────────────────────────────── */
char *reverse_deps(const char *target) {
  static char result[LPM_NAME_MAX * 256];
  result[0] = '\0';

  FILE *db = fopen(LPM_DB, "r");
  if (!db)
    return result;

  char line[MAX_STR];
  while (fgets(line, sizeof(line), db)) {
    line[strcspn(line, "\n")] = '\0';
    char iname[MAX_STR];
    snprintf(iname, MAX_STR, "%s", line);
    char *eq = strchr(iname, '=');
    if (eq)
      *eq = '\0';

    if (strcmp(iname, target) == 0)
      continue;

    char pbfile[LPM_PATH_MAX + LPM_NAME_MAX + 16];
    snprintf(pbfile, sizeof(pbfile), "%s/pkgbuild_%s", LPM_PKGBUILD_DIR, iname);

    char deps[MAX_DEPS][LPM_NAME_MAX];
    int n = bash_array(pbfile, "depends",
                       (char *)deps, LPM_NAME_MAX, MAX_DEPS);
    for (int i = 0; i < n; i++) {
      char depname[LPM_NAME_MAX];
      strncpy(depname, deps[i], sizeof(depname) - 1);
      depname[sizeof(depname) - 1] = '\0';
      /* strip operator */
      char *op = strpbrk(depname, "><=");
      if (op)
        *op = '\0';
      if (strcmp(depname, target) == 0) {
        size_t rlen = strlen(result);
        size_t rsz  = LPM_NAME_MAX * 256;
        if (result[0])
          snprintf(result + rlen, rsz - rlen, " %s", iname);
        else
          snprintf(result, rsz, "%s", iname);
        break;
      }
    }
  }
  fclose(db);
  return result;
}

#pragma GCC diagnostic pop
