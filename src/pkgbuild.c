#include "lpm.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"


/* ── helpers ─────────────────────────────────────────────────────────────── */

/* run bash snippet, collect stdout lines into out[] array, return count */
static int bash_array(const char *pbfile, const char *varname,
                      char out[][LPM_NAME_MAX], int maxn) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "bash -c 'source \"%s\" 2>/dev/null; "
           "for _x in \"${%s[@]}\"; do printf \"%%s\\n\" \"$_x\"; done'",
           pbfile, varname);
  FILE *p = popen(cmd, "r");
  if (!p)
    return 0;
  int n = 0;
  while (n < maxn && fgets(out[n], MAX_STR, p)) {
    out[n][strcspn(out[n], "\n")] = '\0';
    if (out[n][0])
      n++;
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

/* ── is_new_format ───────────────────────────────────────────────────────── *
 * Returns 1 if pbfile uses new format (key = "value"), 0 for bash format.  */
static int is_new_format(const char *pbfile) {
  FILE *f = fopen(pbfile, "r");
  if (!f) return 0;
  char line[512];
  int result = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, " = ") || strncmp(line, "pkgtype", 7) == 0) {
      result = 1; break;
    }
  }
  fclose(f);
  return result;
}

/* ── pkgbuild_parse ──────────────────────────────────────────────────────── *
 * Supports both new format (key = "value") and old bash format.            *
 * For new format: delegates to the C parser (pkgbuild_parse_fast) and      *
 * copies the relevant fields into Pkg — no bash subprocess needed.         *
 * For old format: uses bash_scalar / bash_array as before.                 */
int pkgbuild_parse(const char *pbfile, Pkg *pkg) {
  struct stat st;
  if (stat(pbfile, &st) != 0)
    return -1;

  memset(pkg, 0, sizeof(*pkg));
  strncpy(pkg->pbfile, pbfile, MAX_STR - 1);

  /* ── new format: use C parser ── */
  if (is_new_format(pbfile)) {
    PkgMeta m;
    if (pkgbuild_parse_fast(pbfile, &m) != 0)
      return -1;

    strncpy(pkg->pkgname, m.pkgname, sizeof(pkg->pkgname) - 1);
    strncpy(pkg->pkgver,  m.pkgver,  sizeof(pkg->pkgver)  - 1);
    strncpy(pkg->pkgrel,  m.pkgrel,  sizeof(pkg->pkgrel)  - 1);

    pkg->ndepends = m.ndepends;
    for (int i = 0; i < m.ndepends && i < MAX_DEPS; i++)
      strncpy(pkg->depends[i], m.depends[i], LPM_NAME_MAX - 1);

    pkg->nrecommends = m.nrecommends;
    for (int i = 0; i < m.nrecommends && i < MAX_DEPS; i++)
      strncpy(pkg->recommends[i], m.recommends[i], LPM_NAME_MAX - 1);

    pkg->nmakedepends = m.nmakedepends;
    for (int i = 0; i < m.nmakedepends && i < MAX_DEPS; i++)
      strncpy(pkg->makedepends[i], m.makedepends[i], LPM_NAME_MAX - 1);

    /* source: pkgbuild_parse_fast doesn't parse sources — read directly */
    {
      FILE *f = fopen(pbfile, "r");
      char line[LPM_PATH_MAX];
      while (f && fgets(line, sizeof(line), f) && pkg->nsources < MAX_SRCS) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* match: source  = "url" */
        if (strncmp(p, "source", 6) == 0) {
          char *eq = strchr(p, '=');
          if (!eq) continue;
          char *val = eq + 1;
          while (*val == ' ') val++;
          /* strip surrounding quotes */
          if (*val == '"') val++;
          val[strcspn(val, "\"\n\r")] = '\0';
          if (val[0]) {
            strncpy(pkg->source[pkg->nsources], val, MAX_STR - 1);
            pkg->nsources++;
          }
        }
        /* md5sums = "hash" */
        if (strncmp(p, "md5sums", 7) == 0) {
          char *eq = strchr(p, '=');
          if (!eq) continue;
          char *val = eq + 1;
          while (*val == ' ') val++;
          if (*val == '"') val++;
          val[strcspn(val, "\"\n\r")] = '\0';
          if (val[0]) strncpy(pkg->md5sums[0], val, 32);
        }
        if (strncmp(p, "sha256sums", 10) == 0) {
          char *eq = strchr(p, '=');
          if (!eq) continue;
          char *val = eq + 1;
          while (*val == ' ') val++;
          if (*val == '"') val++;
          val[strcspn(val, "\"\n\r")] = '\0';
          if (val[0]) strncpy(pkg->sha256sums[0], val, 128);
        }
        if (strncmp(p, "sha512sums", 10) == 0) {
          char *eq = strchr(p, '=');
          if (!eq) continue;
          char *val = eq + 1;
          while (*val == ' ') val++;
          if (*val == '"') val++;
          val[strcspn(val, "\"\n\r")] = '\0';
          if (val[0]) strncpy(pkg->sha512sums[0], val, 128);
        }
      }
      if (f) fclose(f);
    }

    pkg->has_check     = m.has_check;
    pkg->has_uninstall = m.has_remove;
    return 0;
  }

  /* ── old bash format ── */
  bash_scalar(pbfile, "pkgname", pkg->pkgname, MAX_STR);
  bash_scalar(pbfile, "pkgver", pkg->pkgver, MAX_STR);
  bash_scalar(pbfile, "pkgrel", pkg->pkgrel, MAX_STR);

  pkg->ndepends = bash_array(pbfile, "depends", pkg->depends, MAX_DEPS);
  pkg->nrecommends =
      bash_array(pbfile, "recommends", pkg->recommends, MAX_DEPS);
  pkg->nmakedepends =
      bash_array(pbfile, "makedepends", pkg->makedepends, MAX_DEPS);
  pkg->nreplaces =
      bash_array(pbfile, "replaces", pkg->replaces, MAX_DEPS);

  bash_scalar(pbfile, "source", pkg->source[0], MAX_STR);
  pkg->nsources = pkg->source[0][0] ? 1 : 0;

  for (int i = 2; i <= MAX_SRCS && pkg->nsources < MAX_SRCS; i++) {
    char varname[16];
    snprintf(varname, sizeof(varname), "source%d", i);
    bash_scalar(pbfile, varname, pkg->source[pkg->nsources], MAX_STR);
    if (pkg->source[pkg->nsources][0])
      pkg->nsources++;
  }

  bash_scalar(pbfile, "sha256sums", pkg->sha256sums[0], MAX_STR);
  for (int i = 2; i <= MAX_SRCS; i++) {
    char varname[32];
    snprintf(varname, sizeof(varname), "sha256sums%d", i);
    bash_scalar(pbfile, varname, pkg->sha256sums[i - 1], MAX_STR);
  }
  bash_scalar(pbfile, "sha512sums", pkg->sha512sums[0], MAX_STR);
  for (int i = 2; i <= MAX_SRCS; i++) {
    char varname[32];
    snprintf(varname, sizeof(varname), "sha512sums%d", i);
    bash_scalar(pbfile, varname, pkg->sha512sums[i - 1], MAX_STR);
  }
  bash_scalar(pbfile, "md5sums", pkg->md5sums[0], MAX_STR);
  for (int i = 2; i <= MAX_SRCS; i++) {
    char varname[32];
    snprintf(varname, sizeof(varname), "md5sums%d", i);
    bash_scalar(pbfile, varname, pkg->md5sums[i - 1], MAX_STR);
  }

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
    int n = bash_array(pbfile, "depends", deps, MAX_DEPS);
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
